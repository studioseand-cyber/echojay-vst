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
// THE MONO CARD'S ROW IS GONE FROM THIS CARD. It was parked in the chrome band
// as a TEMPORARY home until the environment grid existed, and said so. The grid
// now exists and Mono is its first tile, so the card, which is now the codec
// RENDER VIEW reached from the grid's codec tile, no longer carries it; two
// Mono controls on two views would be two stores of one selection. cp PIN4 and
// cp PIN5 are written against these named constants, never against a number,
// so removing the row's 54 px leaves every assertion unchanged and true.
inline constexpr int kCodecChromeH    = 96 + 108;
                                      // header block + footer notice
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

// =============================================================================
//  THE PICTURE GRID: tiles of 3:2 art with a label band below.
// =============================================================================
//
// The same shape as codecCardRows and codecCardHeight above, on purpose: one
// function owns the row count, and the height is derived FROM it, never from
// a second copy of the same division. That is the property cp PIN5 pins for
// the presets and pg PIN3 pins here; it is what stops paint and layout from
// drifting apart.
//
// THE COLUMN COUNT IS DERIVED FROM THE WIDTH, NOT CHOSEN. However many tiles
// of at least kPlaybackTileMinW fit, up to kPlaybackGridMaxCols. There is no
// floor of 2: the floor is 1, to keep the division defined, and the reason
// the count never falls below 2 on any page this plugin can show is the
// arithmetic, which pg PIN1 checks across 400 to 1400 in steps of 10.
//
// THE NUMBERS ARE TIGHTER THAN THEY LOOK. At the smallest page this plugin can
// produce (565 x 405: the minimum window with the A/B bar showing) nine tiles
// fit only at four columns: 133 x 88 tiles, three rows, 360 px. A minimum tile
// width of 134 or more drops that page to three columns and 456 px, which does
// not fit. pg PIN5 is the pin that says so.
inline constexpr int kPlaybackTileMinW    = 130;  // narrowest a tile may be
inline constexpr int kPlaybackTileGap     = 10;   // between tiles, both ways
inline constexpr int kPlaybackTileLabelH  = 22;   // the label band under the art
inline constexpr int kPlaybackGridMaxCols = 4;

/** Columns that fit in availableWidth, 1 to kPlaybackGridMaxCols. */
inline int playbackGridColumns (int availableWidth)
{
    const int fit = (juce::jmax (0, availableWidth) + kPlaybackTileGap)
                  / (kPlaybackTileMinW + kPlaybackTileGap);
    return juce::jlimit (1, kPlaybackGridMaxCols, fit);
}

/** How many rows a tile count occupies at a column count. Exported, like
    codecCardRows, because whatever paints the grid needs this same number. */
inline int playbackGridRows (int tileCount, int columns)
{
    const int c = juce::jmax (1, columns);
    return (juce::jmax (0, tileCount) + c - 1) / c;
}

/** One tile's width at availableWidth, after the gaps between columns. */
inline int playbackTileWidth (int availableWidth)
{
    const int c = playbackGridColumns (availableWidth);
    return juce::jmax (0, (juce::jmax (0, availableWidth) - (c - 1) * kPlaybackTileGap) / c);
}

/** One tile's height: 3:2 art above the label band. */
inline int playbackTileHeight (int availableWidth)
{
    return playbackTileWidth (availableWidth) * 2 / 3 + kPlaybackTileLabelH;
}

/** The grid's height, DERIVED FROM playbackGridRows, the one row count. */
inline int playbackGridHeight (int tileCount, int availableWidth)
{
    return playbackGridRows (tileCount, playbackGridColumns (availableWidth))
         * (playbackTileHeight (availableWidth) + kPlaybackTileGap);
}

/** Tile `index`'s rectangle inside a grid area: the ONE place a tile's
    position is computed, read by paint and, through paint's stored rects, by
    the hit test. */
inline juce::Rectangle<int> playbackTileRect (juce::Rectangle<int> grid, int index)
{
    const int cols = playbackGridColumns (grid.getWidth());
    const int w    = playbackTileWidth   (grid.getWidth());
    const int h    = playbackTileHeight  (grid.getWidth());
    const int i    = juce::jmax (0, index);
    return { grid.getX() + (i % cols) * (w + kPlaybackTileGap),
             grid.getY() + (i / cols) * (h + kPlaybackTileGap), w, h };
}

// =============================================================================
//  THE PLAYBACK PAGE: the grid, and everything that shares the page with it.
// =============================================================================
//
// THE ALLOWANCE IS ONE NAMED SUM OF NAMED PARTS, and the layout below spends
// exactly those parts, so the figure pg PIN5 adds to the grid is the figure
// the paint actually uses rather than a second copy of it. pg PIN5 checks both
// that the sum fits and that the layout spends no more and no less than it.
//
// NO SIDE PADDING, on purpose and not to make anything fit: the page rect is
// already inset 10 px from the column by resized(), on the same edges as the
// reference bar and the sub-tab row above it, so the grid lines up with them.
// Note what an inset would cost: at the smallest page (565 px) any side inset
// over 7 px drops the grid to three columns, and pg PIN5 would say so.
inline constexpr int kPlaybackPagePadTop    = 7;
inline constexpr int kPlaybackPageTitleH    = 22;   // "PLAYBACK", as the card's title row
inline constexpr int kPlaybackPageSubtitleH = 18;
inline constexpr int kPlaybackPageSourceH   = 20;   // the capture line: KEPT, see below
inline constexpr int kPlaybackPageSourceGap = 8;
inline constexpr int kPlaybackPageStatusH   = 16;   // the codec status, when it has text
inline constexpr int kPlaybackPagePadBottom = 7;

/** Everything that shares the page with the grid: 98 px.

    THE SOURCE LINE STAYS because startCodecRender returns silently when there
    is no capture (PluginEditor.cpp, the codecSrcPath_ check at its top): without
    this line the codec path does nothing and says nothing, which this project
    has now found six times. The status line is counted whether or not it has
    text, because it can appear while the grid is showing. */
inline constexpr int kPlaybackPageChromeH = kPlaybackPagePadTop + kPlaybackPageTitleH
                                          + kPlaybackPageSubtitleH + kPlaybackPageSourceH
                                          + kPlaybackPageSourceGap + kPlaybackPageStatusH
                                          + kPlaybackPagePadBottom;

struct PlaybackPageRects
{
    juce::Rectangle<int> title, subtitle, source, grid, status;
};

/** The page's layout, the ONE author of its rects; paint consumes these and
    computes nothing. A page too short for all of it crops the grid and the
    status line from the bottom, never the header. */
inline PlaybackPageRects playbackPageLayout (juce::Rectangle<int> page, int tileCount)
{
    PlaybackPageRects r;
    auto a = page.withTrimmedTop (kPlaybackPagePadTop).withTrimmedBottom (kPlaybackPagePadBottom);
    r.title    = a.removeFromTop (kPlaybackPageTitleH);
    r.subtitle = a.removeFromTop (kPlaybackPageSubtitleH);
    r.source   = a.removeFromTop (kPlaybackPageSourceH);
    a.removeFromTop (kPlaybackPageSourceGap);
    r.grid     = a.removeFromTop (playbackGridHeight (tileCount, a.getWidth()));
    r.status   = a.removeFromTop (kPlaybackPageStatusH);
    return r;
}

} // namespace echojay
