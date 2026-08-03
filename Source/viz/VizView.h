/*
    VizView.h  —  the frame every EchoJay visualisation is drawn inside
    (VISUALS_PLAN.md, "Shared visualization library").

    Every view in Source/viz is a plain juce::Component that takes its data
    through a setter and repaints only when the picture actually moved — the
    contract EedGrMeter established and the reason six open device editors cost
    nothing. This base holds the parts of that which would otherwise be copied
    nine times:

      * the panel — bg3 fill, border2 hairline, the same rounding the GR meter
        and the EQ's graph use, so a curve and a meter sitting side by side read
        as one instrument rather than two widgets,
      * the caption band,
      * the BYPASS dim,
      * and the degenerate-size guard. A rack slot can collapse to 1x1, and the
        editor-paint harness paints every device at 1x1 on purpose. A subclass
        never sees a plot rect it cannot draw in: paintPlot is simply not
        called.

    A subclass implements paintPlot() and nothing else about the look.
*/

#pragma once

#include <JuceHeader.h>
#include "../EchoJayDeviceLookAndFeel.h"

namespace echojay::viz
{

// The palette is the device palette is the plugin palette. Aliased, never
// copied — the same rule EchoJayDeviceLookAndFeel states for Colours.
using Colours = echojay::device::Colours;

using echojay::device::uiFont;

class VizView : public juce::Component
{
public:
    VizView();
    ~VizView() override = default;

    // A short label drawn in the top-left of the frame. Empty for none.
    void setCaption (const juce::String& c);

    // Greys the view out, so a bypassed device is not sitting there displaying
    // a picture of processing it is not doing. Same reason GrMeter has it.
    void setDimmed (bool d);
    bool isDimmed() const noexcept { return dimmed_; }

    // A view that is placed inside another frame (a strip in a bigger graph)
    // turns its own frame off rather than drawing a box inside a box.
    void setFramed (bool f);

    void paint (juce::Graphics& g) override final;

protected:
    // The plot rect is already inset from the frame and is guaranteed to be at
    // least kMinPlot on both axes — below that this is never called.
    virtual void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) = 0;

    // The SAME rect paint() will hand to paintPlot, available outside a paint
    // so a view that caches a juce::Path can build it in resized() rather than
    // rebuilding it every frame. May be empty or degenerate; a caller that is
    // about to compute coordinates checks hasPlot() first.
    juce::Rectangle<float> plotArea() const;
    bool hasPlot() const;

    // Multiply every colour's alpha by this so the whole picture dims together.
    float dimAlpha() const noexcept { return dimmed_ ? 0.30f : 1.0f; }

    // The repaint gate every setter uses: "has this moved enough to be worth a
    // frame?". Spelling it once means nine views cannot drift to nine different
    // ideas of "changed".
    static bool moved (float a, float b, float epsilon) noexcept
    {
        return std::abs (a - b) >= epsilon;
    }

    // Anything narrower than this has no picture in it, only rounding error.
    static constexpr float kMinPlot = 14.0f;

private:
    juce::String caption_;
    bool         dimmed_ = false;
    bool         framed_ = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VizView)
};

} // namespace echojay::viz
