/*
    DwellGlow.cpp  —  see DwellGlow.h.
*/

#include "DwellGlow.h"

#include <cstring>

namespace echojay::viz
{

namespace
{
    // =======================================================================
    // GLOW TUNING — every number that decides how the heat LOOKS, in one block,
    // for every view in the Dynamics family.
    //
    // (One more lives in EedDynamicsCore.h: dyn::kDwellTauSec, how fast energy
    // fades out of the histogram itself. That is the one that decides whether
    // the glow shows where the signal is hitting NOW or a smear of everywhere
    // it has been; the ones below only shape what is already there.)
    // =======================================================================

    // CONTRAST, applied to the normalised 0..1 dwell before it is coloured.
    // Above 1 it pushes the low-dwell tails down and leaves the peak alone, so
    // a signal that spends most of its time in a 6 dB window reads as a core
    // rather than as an even wash over everything it ever touched. 1.0 is the
    // raw histogram; past ~4 it gets so tight it stops showing the spread at
    // all.
    //
    // 2.0 because contrast also decides how much of the core is ALLOWED to be
    // hot: at 2.5 only the very top of the histogram survived into the bright
    // end of the ramp, so the peak was a pinprick. Brightness itself is
    // kGlowPeakAlpha's job, not this one's.
    constexpr float kDwellGamma = 2.0f;

    // VISIBLE FLOOR, as a fraction of the running maximum: below this the field
    // draws nothing. Rescaled rather than hard-cut, so what it removes fades
    // out instead of ending at a rim.
    constexpr float kDwellFloorFrac = 0.09f;

    // BLOB RADIUS — how far the soft field reaches PERPENDICULAR to the shape,
    // in pixels. The profile below has tapered to near nothing by the time it
    // gets here.
    constexpr float kGlowReachPx = 11.0f;

    // PEAK BRIGHTNESS — how hard the field's centre is driven, before the
    // view's own dim/recede.
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

    // A near-vertical shape (a gate's step) would otherwise stretch one
    // column's falloff over the whole plot height. Bounds the work and the
    // smear.
    constexpr float kMaxSlope = 12.0f;

    // An intensity under this cannot make a pixel the eye will find, so a
    // column is skipped rather than composited as a row of 1/255s.
    constexpr float kFaintestVisible = 0.004f;

    // FULL TRIGGER — the share of its time the detector has to spend above a
    // level for fractionAbove to call that level maximally hot.
    //
    // Not 1.0, and this is the difference between the band view glowing and
    // not. A signal is only over a sensibly-set threshold a fraction of the
    // time: measured on voice-shaped material with sibilance under the band,
    // hard de-essing sits near 0.28. Calling 0.30 "as hot as it gets" puts an
    // actually-working de-esser at the top of the ramp; leaving the reference
    // at 1.0 puts it at 4% of it.
    constexpr float kFullTriggerFrac = 0.30f;

    // ---- the colour ramp ---------------------------------------------------
    // The hot end is pure white and it ARRIVES EARLY, at 0.88 rather than at
    // 1.0. Intensity is a smooth hill, so a ramp that only turns white in its
    // last sliver puts white on a couple of pixels and the "white-hot" peak is
    // really just hot amber. Landing white at 0.88 gives the core an area to be
    // white across, which is what reads as heat.
    constexpr float kAmberAt = 0.45f;   // teal has fully become amber by here
    constexpr float kWhiteAt = 0.88f;   // amber has fully become white by here

    // ---- the falloff profile ----------------------------------------------
    // A four-layer bloom collapsed into ONE continuous function of distance
    // from the shape: a tight core plus three progressively wider, fainter
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
}

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------
bool DwellGlow::setSource (const echojay::dyn::DwellTap* tap, bool active)
{
    tap_    = tap;
    active_ = active && tap != nullptr;

    if (tap == nullptr)
    {
        target_.fill (0.0f);
        displayed_.fill (0.0f);
        smoothed_.fill (0.0f);
        cumulative_.fill (0.0f);
        runMax_ = 0.0f;
        alive_  = false;
        return false;
    }

    return active_;
}

// ---------------------------------------------------------------------------
// One frame: read, ease, blur, re-derive the reference.
// ---------------------------------------------------------------------------
bool DwellGlow::tick (float dtSec)
{
    // Clamped at both ends: a zero dt stalls the ease, and a 3-second gap
    // (editor hidden, host stalled, machine asleep) would otherwise snap the
    // glow to its target in one visible jump.
    const float dt = juce::jlimit (0.001f, 0.25f, dtSec);

    // ---- 1: the published target ------------------------------------------
    if (tap_ != nullptr && active_)
    {
        float fresh[kBins];

        // A torn read KEEPS the previous target. One frame of standing still is
        // invisible; a frame of anything else is a flicker, which is the one
        // thing this visual is not allowed to do.
        if (tap_->read (fresh))
            std::copy (fresh, fresh + kBins, target_.begin());
    }
    else
    {
        target_.fill (0.0f);
    }

    // ---- 2: frame-rate-independent easing ---------------------------------
    const float k = 1.0f - std::exp (-dt / kEaseTauSec);

    float motion = 0.0f;

    for (int i = 0; i < kBins; ++i)
    {
        const float d = target_[(std::size_t) i] - displayed_[(std::size_t) i];
        displayed_[(std::size_t) i] += d * k;
        motion = std::max (motion, std::abs (d));
    }

    // ---- 3: spatial smoothing across bins ---------------------------------
    // Two passes of a binomial 5-tap. One pass still leaves the 0.84 dB bin
    // grid faintly readable as steps where the histogram is steep; two does
    // not, and 128 bins twice is nothing.
    {
        std::array<float, (std::size_t) kBins> scratch;

        auto blur = [] (const std::array<float, (std::size_t) kBins>& src,
                        std::array<float, (std::size_t) kBins>& dst)
        {
            for (int i = 0; i < kBins; ++i)
            {
                // Edge-clamped rather than wrapped or zeroed: bin 0 and bin 127
                // are the ends of a dB axis, not neighbours, and darkening them
                // would draw a fade the signal does not have.
                auto at = [&src] (int j) noexcept
                {
                    return src[(std::size_t) juce::jlimit (0, kBins - 1, j)];
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
    // Taken from smoothed_, the SAME array the lookups read, and not from
    // displayed_ before the blur. Blurring lowers the peak of a narrow
    // histogram by a lot, so normalising against the unblurred one divides
    // every intensity by a number the picture never actually contains — and
    // under a contrast of 2 that gap is the difference between a hot core and
    // a shape that never leaves the cold end of the ramp.
    float peak = 0.0f, total = 0.0f;
    for (float v : smoothed_) { peak = std::max (peak, v); total += v; }

    {
        const float tau = peak > runMax_ ? kMaxRiseTauSec : kMaxFallTauSec;
        runMax_ += (peak - runMax_) * (1.0f - std::exp (-dt / tau));
    }

    // ---- the cumulative, for fractionAbove --------------------------------
    // Summed from the TOP down, so cumulative_[b] is the share of the
    // detector's time spent at or above bin b. Built here, once, so a per-pixel
    // "how often does it get this loud" is a lookup and not a 128-bin sum.
    {
        const float inv = total > kQuiet ? 1.0f / total : 0.0f;
        float running = 0.0f;

        for (int i = kBins - 1; i >= 0; --i)
        {
            running += smoothed_[(std::size_t) i];
            cumulative_[(std::size_t) i] = running * inv;
        }
    }

    const bool wasAlive = alive_;
    alive_ = peak > kQuiet || motion > kQuiet;

    // The trailing frame is what actually clears the glow off the screen.
    return alive_ || wasAlive;
}

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------
float DwellGlow::cubicAt (const std::array<float, (std::size_t) kBins>& src,
                          float db) const noexcept
{
    // Position in BIN-CENTRE units.
    const float span = echojay::dyn::kDwellTopDb - echojay::dyn::kDwellFloorDb;
    const float f    = (db - echojay::dyn::kDwellFloorDb) / span * (float) kBins - 0.5f;

    const int   i1 = (int) std::floor (f);
    const float t  = f - (float) i1;

    auto at = [&src] (int j) noexcept
    {
        return src[(std::size_t) juce::jlimit (0, kBins - 1, j)];
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

float DwellGlow::shape (float v) noexcept
{
    // Floor first, then contrast. The floor is RESCALED rather than simply cut,
    // so what it removes fades out instead of ending at a rim; the contrast
    // then pushes what is left of the tails down and leaves the peak where it
    // is. Together they are the difference between "the signal has touched all
    // of this" and "the signal is working HERE".
    v = (v - kDwellFloorFrac) / (1.0f - kDwellFloorFrac);
    return v > 0.0f ? std::pow (juce::jmin (1.0f, v), kDwellGamma) : 0.0f;
}

float DwellGlow::rawAt (float db) const noexcept
{
    return cubicAt (smoothed_, db);
}

float DwellGlow::intensityAt (float db) const noexcept
{
    if (runMax_ <= kQuiet) return 0.0f;
    return shape (rawAt (db) / runMax_);
}

float DwellGlow::fractionAbove (float db) const noexcept
{
    if (runMax_ <= kQuiet) return 0.0f;

    // Referenced to kFullTriggerFrac BEFORE the shared shaping, and it has to
    // be: a fraction-of-time is not a 0..1 quantity the way a normalised
    // density is. Even a de-esser working hard on a vocal only has sibilance
    // under it a quarter to a third of the time, so feeding the raw fraction
    // into a floor of 0.09 and a square leaves a peak around 0.04 — arithmetic
    // that renders heavy de-essing as nothing at all.
    const float f = cubicAt (cumulative_, db) / kFullTriggerFrac;

    return shape (juce::jlimit (0.0f, 1.0f, f));
}

juce::Colour DwellGlow::heatColour (float v)
{
    // The cold end is the palette's own blue, so a silent plot is the colour
    // the shape has always been and the glow reads as that shape heating up
    // rather than as a second graphic laid over it.
    if (v <= kAmberAt)
        return Colours::blue.interpolatedWith (Colours::amber, v / kAmberAt);

    const float t = juce::jmin (1.0f, (v - kAmberAt) / (kWhiteAt - kAmberAt));
    return Colours::amber.interpolatedWith (juce::Colours::white, t);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
int DwellGlow::prepareColumns (juce::Rectangle<float> plot)
{
    // Integer origin so the field lands on the same pixels the cold shape was
    // stroked on; the fractional part is carried into the caller's column maths
    // rather than resampled away by a transformed blit.
    originX_ = (int) std::floor (plot.getX());
    originY_ = (int) std::floor (plot.getY());

    const int w = (int) std::ceil (plot.getRight()  - (float) originX_);
    const int h = (int) std::ceil (plot.getBottom() - (float) originY_);

    if (w < 2 || h < 2) { colY_.clear(); colE_.clear(); return 0; }

    colY_.assign ((std::size_t) w, 0.0f);
    colE_.assign ((std::size_t) w, 0.0f);

    if (image_.getWidth() != w || image_.getHeight() != h)
        image_ = juce::Image (juce::Image::ARGB, w, h, false);

    return w;
}

void DwellGlow::render (juce::Graphics& g, float alpha)
{
    const int w = (int) colE_.size();
    if (w < 2 || alpha <= 0.01f || ! image_.isValid()) return;

    const int h = image_.getHeight();

    // Checked before the image is touched, so a plot with nothing to show never
    // pays to clear one.
    bool any = false;
    for (float e : colE_) if (e > kFaintestVisible) { any = true; break; }
    if (! any) return;

    const float* profile = glowProfile();

    {
        juce::Image::BitmapData bits (image_, juce::Image::BitmapData::writeOnly);

        for (int y = 0; y < h; ++y)
            std::memset (bits.getLinePointer (y), 0,
                         (std::size_t) w * (std::size_t) bits.pixelStride);

        for (int ix = 0; ix < w; ++ix)
        {
            const float e = colE_[(std::size_t) ix];
            if (e <= kFaintestVisible) continue;

            // Slope from the neighbouring columns, so the reach can be
            // stretched to keep the PERPENDICULAR width constant as the shape
            // tilts. Without this the glow visibly thins wherever the shape is
            // steep — and a transfer curve sits at 45 degrees for its whole
            // lower half, so "wherever it is steep" is most of it.
            const int   xl = ix > 0     ? ix - 1 : ix;
            const int   xr = ix < w - 1 ? ix + 1 : ix;
            const float dy = (colY_[(std::size_t) xr] - colY_[(std::size_t) xl])
                           / (float) juce::jmax (1, xr - xl);

            const float slope = juce::jmin (std::abs (dy), kMaxSlope);
            const float reach = kGlowReachPx * std::sqrt (1.0f + slope * slope);

            const juce::Colour c = heatColour (e);
            const float peak = kGlowPeakAlpha * e * alpha;

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

                const float av = peak * profile[(int) (u * (float) kProfileSteps)];
                if (av <= 0.002f) continue;

                // Written PREMULTIPLIED, which is both what juce::Image::ARGB
                // wants and exactly what an additive composite is: colour times
                // coverage. Each column owns its own pixels, so there is nothing
                // to accumulate and nothing to read back.
                auto* px = bits.getPixelPointer (ix, y);
                px[juce::PixelARGB::indexA] = (juce::uint8) juce::jmin (255, (int) (av * 255.0f + 0.5f));
                px[juce::PixelARGB::indexR] = (juce::uint8) juce::jmin (255, (int) (av * cr + 0.5f));
                px[juce::PixelARGB::indexG] = (juce::uint8) juce::jmin (255, (int) (av * cg + 0.5f));
                px[juce::PixelARGB::indexB] = (juce::uint8) juce::jmin (255, (int) (av * cb + 0.5f));
            }
        }
    }

    g.drawImageAt (image_, originX_, originY_);
}

} // namespace echojay::viz
