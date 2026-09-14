#pragma once

#include <JuceHeader.h>

// THE PLAYBACK PAGE'S GEOMETRY, as a pure function.
//
// WHY THIS EXISTS, and it is not tidiness. When Playback became a sub-tab the
// panel kept being laid out by TWO authors: a new site computing the content
// area, and the old modal line 151 lines later in the same resized() setting
// getLocalBounds() unconditionally. The later one won, so the page covered the
// tab strip and the sub-tab row, and with its modal click-swallow still in
// place nothing on the window was clickable. There was no way out: not the
// COMPARE sub-tab, not the tab strip, and not Escape either, because the
// sub-tab path never grabbed keyboard focus.
//
// One function, one computation, and a pin that can exercise it with no window.
// This is the third time in two days that a rectangle computed twice has been
// the defect, and the first two were found by rereading code.
namespace echojay {

struct CodecPageRects
{
    juce::Rectangle<int> page;   // what the component is given
    juce::Rectangle<int> card;   // the panel drawn inside it
};

inline constexpr int kCodecCardMaxW   = 500;
inline constexpr int kCodecCardSideIn = 60;   // breathing room either side
inline constexpr int kCodecRowH       = 58;
inline constexpr int kCodecRowGap     = 8;
inline constexpr int kCodecChromeH    = 96 + 108;   // header block + footer notice
inline constexpr int kCodecCardMinW   = 260;

/** How many rows a preset count occupies, TWO PER ROW. Exported rather than
    kept local, because the paint needs the same number to advance past the grid
    and a second copy of "+ 1) / 2" is how the drop zone's height came to be
    written three different ways. */
inline int codecCardRows (int presetCount)
{
    return (juce::jmax (0, presetCount) + 1) / 2;
}

/** The card's natural height for a preset count. */
inline int codecCardHeight (int presetCount)
{
    return kCodecChromeH + codecCardRows (presetCount) * (kCodecRowH + kCodecRowGap);
}

/** The page fills the content area it is handed, and the card centres inside
    it.

    THE PAGE IS THE CONTENT AREA, NEVER getLocalBounds(). That is the whole
    point: a sub-tab must not be able to cover the row that selects it. The
    card is clamped to the page, so a short window crops the card rather than
    centring it off the top where its header would be unreachable.
*/
inline CodecPageRects codecPageLayout (juce::Rectangle<int> contentArea, int presetCount)
{
    CodecPageRects r;
    r.page = contentArea;

    const int w = juce::jlimit (kCodecCardMinW, kCodecCardMaxW,
                                contentArea.getWidth() - kCodecCardSideIn);
    const int h = juce::jmin (codecCardHeight (presetCount), contentArea.getHeight());

    // ONE MECHANISM, NOT TWO. This line carried a jmax(0, ...) as well, and
    // mutation testing showed the pair made cp PIN3 undetectable: the height
    // clamp above and the jmax each guarantee the card's top stays at or below
    // the page's top, so removing EITHER left the other holding and no single
    // mutation could redden the property. Belt-and-braces on an invariant is
    // belt-and-braces on the TEST of that invariant.
    //
    // The clamp is the one that survives, because it also keeps the card inside
    // the page (cp PIN2), which the jmax never did. With h <= the page height,
    // (height - h) is never negative, so the centring cannot lift the card off
    // the top on its own. Third time today that two fixes for one defect left a
    // pin crediting whichever it happened to reach.
    r.card = { contentArea.getX() + (contentArea.getWidth()  - w) / 2,
               contentArea.getY() + (contentArea.getHeight() - h) / 2,
               w, h };
    return r;
}

} // namespace echojay
