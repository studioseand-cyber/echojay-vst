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
// WHAT FITS, COUNTED WITH THE PAGE'S CHROME. At the smallest page this plugin
// can produce (565 x 373: the minimum window with both bottom bars) the grid
// gets 373 - 114 = 259 px once the header, the note line and the status line
// have their kPlaybackPageChromeH. Its width is the page's less the
// scrollbar's 7 px gutter, 558, so at four columns a tile is 132 x 110 (88 of
// 3:2 art plus the 22 px label band; 133 before the gutter, the same height)
// and a row is 120 with its gap, so two whole rows are visible: eight tiles.
// The widest page is no better, because tile height follows tile width: 1780
// x 1025 also shows eight.
//
// A GRID TALLER THAN ITS AREA SCROLLS; it no longer has to fit. This comment
// used to say nine tiles fit at 565 x 405 in 360 px, which counted the grid
// alone and forgot the chrome (98 px then, 114 since the note line): nine
// needed 458 there. What pg PIN5 now guarantees is that the header, the note
// line and the status line always fit whole and that at least one whole row is
// visible, not that every tile is.
//
// A minimum tile width of 133 or more drops the smallest page's 558 px grid to
// three columns (179 x 141 tiles), which still shows one whole row; the column
// count is pg PIN1's concern, and it is unchanged by scrolling.
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
// reference bar and the sub-tab row above it, so the grid's left edge lines up
// with them. The one inset is the scrollbar's gutter on the right, below.
// Note what an inset costs: at the smallest page (565 px) more than 7 px on
// both sides, or more than 15 px on one, drops the grid to three columns, and
// pg PIN5 would say so.
inline constexpr int kPlaybackPagePadTop    = 7;
inline constexpr int kPlaybackPageTitleH    = 22;   // "PLAYBACK", as the card's title row
inline constexpr int kPlaybackPageSubtitleH = 18;
inline constexpr int kPlaybackPageNoteH     = 16;   // the note: impressions, not measurements
inline constexpr int kPlaybackPageSourceH   = 20;   // the capture line: KEPT, see below
inline constexpr int kPlaybackPageSourceGap = 8;
inline constexpr int kPlaybackPageStatusH   = 16;   // the codec status, when it has text
inline constexpr int kPlaybackPagePadBottom = 7;

/** THE NOTE LINE, under the subtitle (19 Sep 2026). Eight of the page's tiles
    are chosen numbers, and until this line nothing on the page said so: open
    list item 171. One line, and not an apology. 381.6 px in the system face at
    11 pt, measured with CoreText, inside the 565 px of the smallest page. */
inline constexpr const char* kPlaybackPageNote =
    "These are impressions of how a place sounds, not measurements of one.";

/** Everything that shares the page with the grid: 114 px, since the note line
    joined it (98 before).

    THE SOURCE LINE STAYS because startCodecRender returns silently when there
    is no capture (PluginEditor.cpp, the codecSrcPath_ check at its top): without
    this line the codec path does nothing and says nothing, which this project
    has now found six times. The status line is counted whether or not it has
    text, because it can appear while the grid is showing. */
inline constexpr int kPlaybackPageChromeH = kPlaybackPagePadTop + kPlaybackPageTitleH
                                          + kPlaybackPageSubtitleH + kPlaybackPageNoteH
                                          + kPlaybackPageSourceH
                                          + kPlaybackPageSourceGap + kPlaybackPageStatusH
                                          + kPlaybackPagePadBottom;

/** THE SCROLLBAR'S GUTTER (19 Sep 2026), reserved down the grid's right edge
    whether or not the grid scrolls: a 4 px bar at the page's right edge and
    3 px between it and the tiles.

    ALWAYS RESERVED, so the tiles never change width when scrolling engages. A
    gutter taken only when the grid scrolls would narrow the tiles, which
    shortens them, which can make the grid fit again: a window dragged across
    that height would see the tiles jump in width. The cost is 7 px of the
    grid's width everywhere. At the smallest page that costs nothing in height:
    558 px of grid gives 132 px tiles, 110 tall, the same as 565 gave.

    THE TILES CANNOT REACH IT. The grid rect excludes the gutter, a tile's
    stored hit rect is clipped to the grid (playbackTileVisibleRect), and the
    bar is drawn in the gutter only, so a press on the bar is in no tile. */
inline constexpr int kPlaybackScrollBarW      = 4;
inline constexpr int kPlaybackScrollGutterW   = 7;    // the bar and the 3 px beside it
inline constexpr int kPlaybackScrollThumbMinH = 20;   // shortest thumb, however long the grid

struct PlaybackPageRects
{
    juce::Rectangle<int> title, subtitle, note, source, grid, scrollTrack, status;
};

/** The page's layout, the ONE author of its rects; paint consumes these and
    computes nothing.

    THE STATUS LINE IS RESERVED FIRST, and the grid gets what is left. It used
    to be the other way round: the grid took its full height and the status
    line got whatever remained, so a grid taller than the page cropped the
    status line to nothing (and with it the grid's only place for a codec
    error) and ran its lower rows off the bottom. Now the header and the status
    line always fit whole, and the grid area is at most the space between them.

    The grid area is the SMALLER of its content and that space, and the status
    line sits directly under it, so a grid that fits is laid out exactly as it
    was. A grid taller than the space scrolls inside it (playbackTilePlacedRect
    and the functions below).

    A LAST ROW SHORT BY LESS THAN THE BOTTOM PAD TAKES THE PAD (19 Sep 2026).
    Without this, a page a few pixels short of the grid gave it a scroll of a
    few pixels. At 741 x 553, the default window, the grid got 439 px for
    tiles ending at 440: a 1 px scroll, and the page announced "2 more below"
    over two tiles whose only missing pixel was the bottom of their outline.
    Now, when the last row's bottom is past the room by no more than
    kPlaybackPagePadBottom, the grid is given exactly the room to that bottom
    and the status line moves down into the pad, still whole and still inside
    the page. So the grid scrolls by 0, or by more than the pad, never by 1 to
    7 px, and a scrollbar never appears over nothing (pg PIN9). */
inline PlaybackPageRects playbackPageLayout (juce::Rectangle<int> page, int tileCount)
{
    PlaybackPageRects r;
    auto a = page.withTrimmedTop (kPlaybackPagePadTop);
    r.title    = a.removeFromTop (kPlaybackPageTitleH);
    r.subtitle = a.removeFromTop (kPlaybackPageSubtitleH);
    r.note     = a.removeFromTop (kPlaybackPageNoteH);
    r.source   = a.removeFromTop (kPlaybackPageSourceH);
    a.removeFromTop (kPlaybackPageSourceGap);

    const int gridW    = juce::jmax (0, a.getWidth() - kPlaybackScrollGutterW);
    const int content  = playbackGridHeight (tileCount, gridW);
    const int lastEdge = juce::jmax (0, content - kPlaybackTileGap);   // the last row's bottom
    const int gridRoom = juce::jmax (0, a.getHeight() - kPlaybackPageStatusH
                                          - kPlaybackPagePadBottom);     // status first
    int gridH = juce::jmin (content, gridRoom);
    if (lastEdge > gridRoom && lastEdge - gridRoom <= kPlaybackPagePadBottom)
        gridH = lastEdge;                                                // take the pad

    auto row      = a.removeFromTop (gridH);
    r.scrollTrack = row.removeFromRight (juce::jmin (kPlaybackScrollGutterW, row.getWidth()))
                       .removeFromRight (kPlaybackScrollBarW);
    r.grid        = row;
    r.status      = a.removeFromTop (kPlaybackPageStatusH);
    return r;
}

// =============================================================================
//  THE GRID'S SCROLL: one offset, and the arithmetic every use of it shares.
// =============================================================================
//
// The Playback page is one component that computes its rects in paint and
// hit-tests the stored ones, so the scroll is a manual offset rather than a
// Viewport, which would split rect computation and hit testing across two
// classes. The offset is applied in ONE place, playbackTilePlacedRect; paint
// draws a tile at that rect and stores what is VISIBLE of it for the hit test,
// so a tile scrolled under the header or the status line cannot be pressed
// where it cannot be seen, and mouseUp needs no offset of its own.

/** How far the grid can scroll: until the last row's BOTTOM meets the grid
    area's bottom, or 0 when every tile is already wholly visible.

    NOT playbackGridHeight less the area. That height includes the gap after
    the last row, so it would leave up to a gap's worth of scroll over nothing
    at all, and a grid could scroll while no tile was hidden. Measured to the
    last tile's edge, "can scroll" and "part of a tile is out of sight" are the
    same statement. With the layout's pad rule above, the answer is 0 or more
    than kPlaybackPagePadBottom, never a few pixels (pg PIN9). */
inline int playbackGridMaxScroll (int tileCount, juce::Rectangle<int> grid)
{
    const int content = playbackGridHeight (tileCount, grid.getWidth());
    if (content <= 0) return 0;
    return juce::jmax (0, content - kPlaybackTileGap - grid.getHeight());
}

/** An offset clamped to what the grid can scroll: never above the top, never
    past the last row. */
inline int playbackClampScroll (int scroll, int tileCount, juce::Rectangle<int> grid)
{
    return juce::jlimit (0, playbackGridMaxScroll (tileCount, grid), scroll);
}

/** Tile `index` where it sits after scrolling, at full size. THE ONE PLACE THE
    OFFSET IS APPLIED. Paint draws the tile here, clipped to the grid. */
inline juce::Rectangle<int> playbackTilePlacedRect (juce::Rectangle<int> grid, int index, int scroll)
{
    return playbackTileRect (grid, index).translated (0, -scroll);
}

/** What is VISIBLE of tile `index`: its placed rect clipped to the grid area.
    Empty when it is scrolled wholly out. This is what the hit test stores. */
inline juce::Rectangle<int> playbackTileVisibleRect (juce::Rectangle<int> grid, int index, int scroll)
{
    return playbackTilePlacedRect (grid, index, scroll).getIntersection (grid);
}

/** THE SCROLL SAYS SO: the scrollbar's thumb in the layout's scrollTrack, and
    an EMPTY rect whenever the grid cannot scroll, which is the one test paint
    makes before drawing the bar. A grid with more below it and nothing saying
    so reads as all the tiles there are, which is a refusal rendered as an
    absence.

    The thumb is the track's height scaled by what is visible of the grid's
    tiles, visible / (visible + max scroll), no shorter than
    kPlaybackScrollThumbMinH, and it travels the rest of the track in
    proportion to the offset: at the top at 0, on the track's bottom at the
    maximum. The offset is clamped first, like every other reader of it. */
inline juce::Rectangle<int> playbackScrollThumb (int tileCount, juce::Rectangle<int> grid,
                                                 juce::Rectangle<int> track, int scroll)
{
    const int maxS = playbackGridMaxScroll (tileCount, grid);
    if (maxS <= 0 || track.isEmpty())
        return {};
    const int s      = juce::jlimit (0, maxS, scroll);
    const int trackH = track.getHeight();
    const int prop   = (int) ((juce::int64) trackH * grid.getHeight() / (grid.getHeight() + maxS));
    const int h      = juce::jlimit (juce::jmin (kPlaybackScrollThumbMinH, trackH), trackH, prop);
    const int y      = track.getY() + (int) ((juce::int64) (trackH - h) * s / maxS);
    return { track.getX(), y, track.getWidth(), h };
}

/** Pixels to scroll for a wheel or trackpad delta. Positive deltaY scrolls
    toward the top.

    224 px per unit of delta is CHOSEN, not measured: it is the scale JUCE's own
    Viewport applies by default as I recall it (14 times a 16 px step), not read
    from JUCE's source here. Nobody has watched it on this page, because with
    seven tiles the grid never scrolls; it is tuned the first time a build has
    more than eight. A nonzero delta always moves at least one pixel, so a slow
    trackpad cannot round to nothing. */
inline constexpr float kPlaybackWheelPxPerUnit = 224.0f;

inline int playbackWheelStepPx (float deltaY)
{
    if (deltaY == 0.0f) return 0;
    const float px = deltaY * kPlaybackWheelPxPerUnit;
    return px > 0.0f ? juce::jmax (1, juce::roundToInt (px)) : juce::jmin (-1, juce::roundToInt (px));
}

} // namespace echojay
