#pragma once
// EchoJayScrollbarStyle (17 Sep 2026, rides 17e): the ONE setting for the horizontal
// scrollbars under the chain strip (Chain tab, ChainListPanel::stripView) and under
// the Link rack row (Link tab, linkMixerViewport_). Both viewports draw through
// EchoJayLookAndFeel::drawScrollbar, which used to paint the thumb in Colours::bg4
// (0xff141626, a hair off the 0xff0E1020 panel) at 0.4 alpha and no track at all -
// the ScrollBar::thumbColourId/trackColourId set on the LookAndFeel are not read by
// a custom drawScrollbar, so they never applied. Thumb: a visible mid-grey with a
// lighter hover state; track: one step lighter than the panel; thumb thickness
// never under 8 px. tools/scrollbar_contrast_guard reads the thumb off the REAL
// component and asserts contrast >= 3:1 against the panel.
#include <JuceHeader.h>

namespace echojay
{
struct ScrollbarStyle
{
    static inline const juce::Colour thumb      { 0xff6b6f86 };   // mid-grey
    static inline const juce::Colour thumbHover { 0xff9a9eb8 };   // lighter on hover
    static inline const juce::Colour track      { 0xff1a1d30 };   // one step lighter than the panel (bg3 0xff0E1020)
    static constexpr int   thickness       = 10;   // the bar's thickness, both viewports
    static constexpr float minThumbPx      = 8.0f; // the thumb never thinner than this
};

// WCAG relative luminance / contrast ratio, for the guard and for anyone tuning the colours.
inline double relativeLuminance (juce::Colour c)
{
    auto lin = [] (double v) { return v <= 0.03928 ? v / 12.92 : std::pow ((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin (c.getFloatRed()) + 0.7152 * lin (c.getFloatGreen()) + 0.0722 * lin (c.getFloatBlue());
}
inline double contrastRatio (juce::Colour a, juce::Colour b)
{
    const double la = relativeLuminance (a), lb = relativeLuminance (b);
    return (juce::jmax (la, lb) + 0.05) / (juce::jmin (la, lb) + 0.05);
}
} // namespace echojay
