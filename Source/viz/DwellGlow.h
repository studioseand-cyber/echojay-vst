/*
    DwellGlow.h  —  the dwell heatmap, factored out of TransferCurveView so the
    Dynamics family has ONE glow rather than several that look alike.

    The transfer curve grew this: a 128-bin dwell histogram published from the
    audio thread (dyn::DwellTap), eased with a time constant, blurred across
    bins, and composited per pixel as a soft cold-to-white-hot field along a
    shape. The de-esser's band view wants exactly the same treatment on a
    different shape, and "exactly the same" is the requirement — two glows tuned
    separately are two glows that drift apart on the first nudge, and the whole
    point of the family reading as one system is that a compressor and a
    de-esser are visibly the same instrument.

    So the tuning constants, the colour ramp, the interpolation and the
    compositor live here, once, and a view supplies only the two things that are
    genuinely its own:

      * WHERE the glow goes — a y per pixel column, its own shape,
      * HOW HOT it is there — an intensity per pixel column, its own question.

    The transfer curve asks "how much time does the detector spend at the level
    this x stands for" (intensityAt). The band view asks "how often does content
    at this frequency actually drive the detector past its threshold"
    (fractionAbove). Different questions, same histogram, same picture language.

    NOT a Component and NOT a Timer. A view owns its own timer and calls tick()
    — because whether to run at all is a question about the view (is it
    visible, is the device bypassed), not about the glow.
*/

#pragma once

#include "VizView.h"
#include "../EedDynamicsCore.h"

#include <array>
#include <vector>

namespace echojay::viz
{

class DwellGlow
{
public:
    DwellGlow() = default;

    static constexpr int kBins = echojay::dyn::kDwellBins;

    // The rate a view should run its timer at. 60 rather than the editor's 20
    // because the whole claim of this visual is that it FLOWS, and 20 Hz is the
    // rate at which a moving gradient becomes a sequence of gradients.
    static constexpr int kGlowHz = 60;

    // The shape's own line, under the glow: cold teal, drawn whatever the
    // histogram says, so a silent or bypassed plot still reads as a diagram.
    static constexpr float kColdAlpha = 0.55f;

    // ---- wiring ------------------------------------------------------------
    // `active` false fades the glow out over the easing time constant rather
    // than snapping it off; nullptr detaches and clears immediately, because a
    // detached source means nobody is watching and a frozen glow would be a
    // claim about a signal that is no longer being read.
    //
    // Returns true if the caller should (re)start its timer.
    bool setSource (const echojay::dyn::DwellTap* tap, bool active);

    bool isActive() const noexcept { return active_; }

    // ---- one frame ---------------------------------------------------------
    // Reads the tap, eases, blurs, and re-derives the normalisation reference.
    // `dtSec` is MEASURED elapsed time — the reason this eases with a time
    // constant instead of a fixed multiply is to survive the frames the message
    // thread does not give us, and assuming 1/60 here would throw that away.
    //
    // Returns true if this frame is worth a repaint.
    bool tick (float dtSec);

    // Is there anything left to draw? A view stops its timer when this goes
    // false AND its source is inactive.
    bool isAlive() const noexcept { return alive_; }

    // ---- what the histogram says -------------------------------------------
    // All three interpolate CUBICALLY between bin centres. A plot shows about
    // 30 dB, which is only ~35 of the 128 bins, so a pixel-resolution walk asks
    // for something like ten samples between every pair of centres — exactly
    // the regime where linear interpolation creases at each centre.

    // The smoothed histogram at a level, un-normalised.
    float rawAt (float db) const noexcept;

    // 0..1 heat for a level: rawAt over the running maximum, floored, then
    // contrast-shaped. "How much of its time does the signal spend HERE" —
    // the transfer curve's question, where x IS a level.
    float intensityAt (float db) const noexcept;

    // 0..1 heat for a level, from the fraction of the detector's time spent AT
    // OR ABOVE it, floored and contrast-shaped the same way. "How often does it
    // get at least this loud" — the band view's question, where the level being
    // asked about is what a given frequency would have to reach to trigger.
    //
    // Monotonic, which the point lookup above is not, and that matters: a band
    // whose skirts happened to sit on the histogram's peak would otherwise
    // light up brighter than its own centre, drawing a ring instead of a core.
    float fractionAbove (float db) const noexcept;

    // Cold teal -> amber -> true white. Shared so the two views cannot end up
    // with two ramps.
    static juce::Colour heatColour (float v);

    // ---- rendering ---------------------------------------------------------
    // A view fills one y and one intensity per PIXEL COLUMN and then renders.
    // Per pixel and not per segment because the first version of this stroked
    // flat-coloured segments and read as blocks: the histogram's bins are wider
    // than the strokes could ever be fine, so every join between two strokes
    // was an edge, and the eye finds edges.
    //
    // Sizes the column buffers and returns how many there are, 0 if the plot is
    // too small to draw in. columnY is measured from the plot's integer top.
    int prepareColumns (juce::Rectangle<float> plot);

    std::vector<float>& columnY()         noexcept { return colY_; }
    std::vector<float>& columnIntensity() noexcept { return colE_; }

    // Composites a soft falloff around the column line into an image and blits
    // it once. `alpha` is the view's own dim/recede multiplier.
    void render (juce::Graphics& g, float alpha);

private:
    // ---- the easing, which is the view-independent half of the smoothness ---
    // How long the displayed histogram takes to cover ~63% of the distance to
    // the published one. 120 ms is what makes the glow feel attached to the
    // music: shorter and it inherits the jitter of the block rate, longer and
    // it lags the phrase you are listening to.
    static constexpr float kEaseTauSec = 0.120f;

    // The self-scaling reference rises fast (a new peak IS the new maximum) and
    // falls slower, so the picture does not re-normalise into a blaze every
    // time the music thins out for a beat.
    //
    // The fall cannot be much slower than this, and that is not a taste call.
    // Everything is divided by this number and then raised to the contrast, so
    // a reference still sitting 30% above the current peak does not dim the
    // core by 30%, it dims it by more than half — the glow goes cold while the
    // signal is still playing.
    static constexpr float kMaxRiseTauSec = 0.25f;
    static constexpr float kMaxFallTauSec = 0.65f;

    // Below this the histogram is nothing, and nothing is not worth a frame.
    static constexpr float kQuiet = 1.0e-4f;

    float cubicAt (const std::array<float, (std::size_t) kBins>& src,
                   float db) const noexcept;

    static float shape (float v) noexcept;

    const echojay::dyn::DwellTap* tap_ = nullptr;
    bool active_ = false;
    bool alive_  = false;

    // target_ is what the audio thread published; displayed_ eases toward it;
    // smoothed_ is displayed_ blurred across bins and is the only one the
    // lookups read. Three arrays rather than one because each layer of
    // smoothing runs at its own rate: easing and blurring are per frame,
    // publishing is per 256 samples.
    std::array<float, (std::size_t) kBins> target_    {};
    std::array<float, (std::size_t) kBins> displayed_ {};
    std::array<float, (std::size_t) kBins> smoothed_  {};

    // Running sum from the top down, normalised, rebuilt with the blur. Only
    // fractionAbove reads it, and building it once per frame is what makes that
    // a lookup rather than a 128-bin sum per pixel.
    std::array<float, (std::size_t) kBins> cumulative_ {};

    float runMax_ = 0.0f;

    // The glow is composited here and drawn in one blit. Kept across frames —
    // reallocating a plot-sized image sixty times a second is the one thing
    // that would make a per-pixel field more expensive than strokes.
    juce::Image        image_;
    std::vector<float> colY_, colE_;
    int   originX_ = 0, originY_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DwellGlow)
};

} // namespace echojay::viz
