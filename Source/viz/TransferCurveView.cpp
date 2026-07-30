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

    // =======================================================================
    // GLOW TUNING — the numbers worth nudging, in one block.
    //
    // (A fifth lives in EedDynamicsCore.h: dyn::kDwellTauSec, how fast energy
    // fades out of the histogram itself. That is the one that decides whether
    // the glow shows where the signal is hitting NOW or a smear of everywhere
    // it has been; the ones below only shape what is already there.)
    // =======================================================================

    // CONTRAST, applied to the normalised 0..1 dwell before it is coloured.
    // Above 1 it pushes the low-dwell tails down and leaves the peak alone, so
    // a signal that spends most of its time in a 6 dB window reads as a core
    // rather than as an even wash over everything it ever touched. 1.0 is the
    // raw histogram; past ~4 it gets so tight it stops showing the spread at all.
    //
    // 2.0 rather than 2.5, because contrast also decides how much of the core
    // is ALLOWED to be hot: at 2.5 only the very top of the histogram survived
    // into the bright end of the ramp, so the peak was a pinprick. Two is still
    // a clear core and gives it something to be bright ACROSS. Brightness
    // itself is kGlowPeakAlpha's job, not this one's.
    constexpr float kDwellGamma = 2.0f;

    // VISIBLE FLOOR, as a fraction of the running maximum: below this the field
    // draws nothing. Rescaled rather than hard-cut, so what it removes fades
    // out instead of ending at a rim.
    constexpr float kDwellFloorFrac = 0.09f;

    // BLOB RADIUS — how far the soft field reaches PERPENDICULAR to the curve,
    // in pixels. The profile below has tapered to near nothing by the time it
    // gets here.
    constexpr float kGlowReachPx = 11.0f;

    // PEAK BRIGHTNESS — how hard the field's centre is driven, before the
    // plot's own dim/recede.
    //
    // DELIBERATELY OVER 1, which is what makes the core bloom rather than just
    // reach full opacity at one pixel and stop. The composite clamps at 255, so
    // an overdrive of 1.8 means every pixel whose falloff is above 1/1.8 lands
    // fully opaque: the hot core becomes a small SOLID region with the bloom
    // falling away outside it, instead of a single bright line. It is the same
    // thing a blown-out highlight does in a photograph, and it is why the
    // brightest part of a real glow looks like an area and not an edge.
    //
    // Raising this does not widen the glow — the falloff still ends where
    // kGlowReachPx says. It only decides how much of that falloff saturates.
    constexpr float kGlowPeakAlpha = 1.8f;

    // The curve as a DIAGRAM, under the glow. Cold teal, drawn whatever the
    // histogram says, so a silent or bypassed plot still shows its shape.
    constexpr float kColdCurveAlpha = 0.55f;

    // A near-vertical curve (a gate's step) would otherwise stretch one
    // column's falloff over the whole plot height. Bounds the work and the smear.
    constexpr float kMaxSlope = 12.0f;

    // ---- the falloff profile ----------------------------------------------
    // The four-layer bloom, collapsed into ONE continuous function of distance
    // from the curve: a tight core plus three progressively wider, fainter
    // gaussians. Layered STROKES could only sample this at their own widths,
    // which is what made the first version read as bands; as a profile it is
    // evaluated per pixel, so the falloff is smooth by construction.
    constexpr int kProfileSteps = 256;

    const float* glowProfile()
    {
        static const std::vector<float> table = []
        {
            std::vector<float> t ((std::size_t) kProfileSteps + 1, 0.0f);

            auto gauss = [] (float d, float sigma)
            {
                return std::exp (-(d * d) / (2.0f * sigma * sigma));
            };

            for (int i = 0; i <= kProfileSteps; ++i)
            {
                const float d = (float) i / (float) kProfileSteps * kGlowReachPx;

                t[(std::size_t) i] = 1.00f * gauss (d, 0.85f)    // core
                                   + 0.42f * gauss (d, 2.10f)    // inner bloom
                                   + 0.17f * gauss (d, 4.20f)    // mid bloom
                                   + 0.07f * gauss (d, 7.50f);   // outer bloom
            }

            const float norm = 1.0f / t[0];
            for (auto& v : t) v *= norm;

            return t;
        }();

        return table.data();
    }

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

    float motion = 0.0f;

    for (int i = 0; i < kDwellBins; ++i)
    {
        const float d = target_[(std::size_t) i] - displayed_[(std::size_t) i];
        displayed_[(std::size_t) i] += d * k;

        motion = std::max (motion, std::abs (d));
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

    // ---- the self-scaling reference ---------------------------------------
    // Taken from smoothed_, the SAME array the render samples, and not from
    // displayed_ before the blur. Blurring lowers the peak of a narrow
    // histogram by a lot, so normalising against the unblurred one divides
    // every intensity by a number the picture never actually contains — and
    // under a contrast of 2.5 that gap is the difference between a hot core and
    // a curve that never leaves the cold end of the ramp at all.
    float peak = 0.0f;
    for (float v : smoothed_) peak = std::max (peak, v);

    {
        const float tau = peak > runMax_ ? kMaxRiseTauSec : kMaxFallTauSec;
        runMax_ += (peak - runMax_) * (1.0f - std::exp (-dtc / tau));
    }

    // ---- repaint only while there is something to see ---------------------
    const bool alive = peak > kQuiet || motion > kQuiet;

    if (alive || wasAlive_) repaint();     // the trailing frame clears the glow
    wasAlive_ = alive;

    // Faded out AND told it is bypassed: nothing will change again until the
    // editor says otherwise, and it says so 20 times a second.
    if (! alive && ! dwellActive_) stopTimer();
}

float TransferCurveView::dwellRaw (float inDb) const noexcept
{
    // Position in BIN-CENTRE units. A plot shows about 30 dB, which is only
    // ~35 of the 128 bins, so a pixel-resolution walk asks for something like
    // ten samples between every pair of centres — exactly the regime where
    // linear interpolation shows a crease at each centre and cubic does not.
    const float span = echojay::dyn::kDwellTopDb - echojay::dyn::kDwellFloorDb;
    const float f    = (inDb - echojay::dyn::kDwellFloorDb) / span * (float) kDwellBins - 0.5f;

    const int   i1 = (int) std::floor (f);
    const float t  = f - (float) i1;

    auto at = [this] (int j) noexcept
    {
        return smoothed_[(std::size_t) juce::jlimit (0, kDwellBins - 1, j)];
    };

    // Catmull-Rom through the four nearest centres, clamped at zero: an
    // overshooting spline can dip negative between two bins, and a negative
    // intensity would be a dark notch through the middle of a bright core.
    const float p0 = at (i1 - 1), p1 = at (i1), p2 = at (i1 + 1), p3 = at (i1 + 2);

    const float a0 = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    const float a1 =         p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    const float a2 = -0.5f * p0             + 0.5f * p2;

    return std::max (0.0f, ((a0 * t + a1) * t + a2) * t + p1);
}

float TransferCurveView::dwellIntensity (float inDb) const noexcept
{
    float v = 0.0f;

    if (runMax_ > kQuiet)
    {
        v = dwellRaw (inDb) / runMax_;

        // Floor first, then contrast. The floor is RESCALED rather than simply
        // cut, so what it removes fades out instead of ending at a rim; the
        // contrast then pushes what is left of the tails down and leaves the
        // peak where it is. Together they are the difference between "the
        // signal has touched all of this" and "the signal is working HERE".
        v = (v - kDwellFloorFrac) / (1.0f - kDwellFloorFrac);
        v = v > 0.0f ? std::pow (v, kDwellGamma) : 0.0f;
    }

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

juce::Colour TransferCurveView::dwellColour (float v)
{
    // Cold teal -> amber -> TRUE WHITE. The cold end is the palette's own blue,
    // so a silent plot is the same colour the curve has always been and the
    // glow reads as that curve heating up rather than as a second graphic.
    //
    // The hot end is pure 255/255/255 and it ARRIVES EARLY, at 0.88 rather than
    // at 1.0. A ramp that only reaches white in its last sliver puts white on
    // nothing: intensity is a smooth hill, so the top 12% of it is a couple of
    // pixels wide and the "white-hot" peak was really just hot amber. Landing
    // white at 0.88 gives the peak an area to be white across, which is what
    // reads as a hot core.
    constexpr float kAmberAt = 0.45f;   // teal has fully become amber by here
    constexpr float kWhiteAt = 0.88f;   // amber has fully become white by here

    if (v <= kAmberAt)
        return Colours::blue.interpolatedWith (Colours::amber, v / kAmberAt);

    const float t = juce::jmin (1.0f, (v - kAmberAt) / (kWhiteAt - kAmberAt));
    return Colours::amber.interpolatedWith (juce::Colours::white, t);
}

// ---------------------------------------------------------------------------
// The heat, as a DENSE FIELD rather than as strokes.
//
// The first version of this stroked the curve in short segments, each flat-
// coloured from the dwell at its midpoint, and it read as blocks. The
// arithmetic says why: the histogram's 128 bins span 108 dB, a plot shows
// about 30 of those dB, so only ~35 bins cover the whole width — and a bloom
// layer subdivided into twenty strokes painted each one as a ~17 px slab of a
// single colour. Every boundary between two slabs was an edge, and the eye
// finds edges.
//
// So this walks PIXEL COLUMNS instead. For each column: the input dB it stands
// at, the curve's y and slope there, and the intensity from a CUBIC lookup
// between bins — all continuous, so no two neighbouring columns can differ by
// a step. The soft falloff is composited straight into an ARGB image and
// blitted once, which is both a genuinely per-pixel gradient and CHEAPER than
// the strokes were: no path construction, no rasteriser setup, no overdraw.
//
// The falloff runs PERPENDICULAR to the curve, not vertically, which is what
// the slope compensation is for. Without it the glow visibly thins wherever
// the curve is steep — and on a transfer plot the curve sits at 45 degrees for
// its whole lower half, so "wherever it is steep" is most of it.
// ---------------------------------------------------------------------------
void TransferCurveView::renderGlow (juce::Graphics& g, juce::Rectangle<float> plot, float a)
{
    const int n = (int) curvePts_.size();
    if (n < 2 || a <= 0.01f) return;

    // Integer origin, so the field lands on the same pixels the cold curve was
    // stroked on; the fractional part is carried into the column maths rather
    // than resampled away by a transformed blit.
    const float ox = std::floor (plot.getX());
    const float oy = std::floor (plot.getY());

    const int w = (int) std::ceil (plot.getRight()  - ox);
    const int h = (int) std::ceil (plot.getBottom() - oy);
    if (w < 2 || h < 2) return;

    colE_.assign ((std::size_t) w, 0.0f);
    colY_.assign ((std::size_t) w, 0.0f);

    // ---- pass one: intensity and curve-y per column -----------------------
    // Before touching the image, so a plot with nothing to show never pays for
    // one at all.
    bool any = false;

    for (int ix = 0; ix < w; ++ix)
    {
        // Where this column sits along the curve, in the same parameterisation
        // rebuildPath used — so the field cannot drift off the line it lights.
        const float t   = (ox + (float) ix + 0.5f - plot.getX()) / plot.getWidth();
        const float pos = juce::jlimit (0.0f, 1.0f, t) * (float) (n - 1);

        const int   i0   = juce::jlimit (0, n - 2, (int) pos);
        const float frac = pos - (float) i0;

        const auto& p0 = curvePts_[(std::size_t) i0];
        const auto& p1 = curvePts_[(std::size_t) i0 + 1];

        colY_[(std::size_t) ix] = (p0.y + (p1.y - p0.y) * frac) - oy;

        const float inDb = curveInDb_[(std::size_t) i0]
                         + (curveInDb_[(std::size_t) i0 + 1]
                          - curveInDb_[(std::size_t) i0]) * frac;

        const float e = dwellIntensity (inDb);
        colE_[(std::size_t) ix] = e;

        if (e > kFaintestVisible) any = true;
    }

    if (! any) return;

    if (glowImage_.getWidth() != w || glowImage_.getHeight() != h)
        glowImage_ = juce::Image (juce::Image::ARGB, w, h, false);

    const float* profile = glowProfile();

    {
        juce::Image::BitmapData bits (glowImage_, juce::Image::BitmapData::writeOnly);

        for (int y = 0; y < h; ++y)
            std::memset (bits.getLinePointer (y), 0,
                         (std::size_t) w * (std::size_t) bits.pixelStride);

        for (int ix = 0; ix < w; ++ix)
        {
            const float e = colE_[(std::size_t) ix];
            if (e <= kFaintestVisible) continue;

            // Slope from the neighbouring columns, so the reach can be stretched
            // to keep the PERPENDICULAR width constant as the curve tilts.
            const int   xl = ix > 0     ? ix - 1 : ix;
            const int   xr = ix < w - 1 ? ix + 1 : ix;
            const float dy = (colY_[(std::size_t) xr] - colY_[(std::size_t) xl])
                           / (float) juce::jmax (1, xr - xl);

            const float slope = juce::jmin (std::abs (dy), kMaxSlope);
            const float reach = kGlowReachPx * std::sqrt (1.0f + slope * slope);

            const juce::Colour c = dwellColour (e);
            const float peak = kGlowPeakAlpha * e * a;

            const float cy = colY_[(std::size_t) ix];
            const int   y0 = juce::jmax (0,     (int) std::floor (cy - reach));
            const int   y1 = juce::jmin (h - 1, (int) std::ceil  (cy + reach));

            const float rInv = 1.0f / reach;
            const float cr = (float) c.getRed();
            const float cg = (float) c.getGreen();
            const float cb = (float) c.getBlue();

            for (int y = y0; y <= y1; ++y)
            {
                const float u = std::abs (((float) y + 0.5f) - cy) * rInv;
                if (u >= 1.0f) continue;

                const float alpha = peak * profile[(int) (u * (float) kProfileSteps)];
                if (alpha <= 0.002f) continue;

                // Written PREMULTIPLIED, which is both what juce::Image::ARGB
                // wants and exactly what an additive composite is: colour times
                // coverage, summed. Each column owns its own pixels, so there is
                // nothing to accumulate and nothing to read back.
                auto* px = bits.getPixelPointer (ix, y);
                px[juce::PixelARGB::indexA] = (juce::uint8) juce::jmin (255, (int) (alpha * 255.0f + 0.5f));
                px[juce::PixelARGB::indexR] = (juce::uint8) juce::jmin (255, (int) (alpha * cr + 0.5f));
                px[juce::PixelARGB::indexG] = (juce::uint8) juce::jmin (255, (int) (alpha * cg + 0.5f));
                px[juce::PixelARGB::indexB] = (juce::uint8) juce::jmin (255, (int) (alpha * cb + 0.5f));
            }
        }
    }

    g.drawImageAt (glowImage_, (int) ox, (int) oy);
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
        g.setColour (Colours::blue2.withMultipliedAlpha (kColdCurveAlpha * a));
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
