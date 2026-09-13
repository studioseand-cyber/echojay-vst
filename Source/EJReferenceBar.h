#pragma once

#include <JuceHeader.h>

// THE REFERENCE BAR: geometry and stepping, as pure functions.
//
// The bar replaces the preset row and the 106px drop zone at the top of
// Compare, 164 pixels of chrome returned to the waveforms. Its shape is
// Faster Master's title row:
//
//   prev | next | play | the current reference's name | status | Browse | + Add
//
// WHY THIS IS PINNED WHEN layoutFor IS NOT. Two geometry defects in one day
// came from a rectangle computed twice and the copies disagreeing: a drop-zone
// height written three ways, and a strip measured against a box it was not
// drawn in. Neither was visible to any gate. This bar is laid out ONCE here,
// and because the function is pure a pin can exercise it at every window width
// with no window at all. That is the difference between the defect class being
// impossible and being documented.
namespace echojay {

struct RefBarRects
{
    juce::Rectangle<int> bar, prev, next, play, name, status, browse, add;
};

inline constexpr int kRefBarH        = 30;
inline constexpr int kRefBarGap      = 4;
// The whole band the bar occupies in the Compare accumulator, replacing
// 26 + (kRefDropH + 4) = 136.
inline constexpr int kRefBarBandH    = kRefBarH + kRefBarGap;
inline constexpr int kRefBarBtnW     = 26;   // prev, next
inline constexpr int kRefBarPlayW    = 30;
inline constexpr int kRefBarBrowseW  = 74;
inline constexpr int kRefBarAddW     = 96;
inline constexpr int kRefBarStatusW  = 180;
inline constexpr int kRefBarPad      = 6;
inline constexpr int kRefBarNameMinW = 40;

/** The narrowest bar this function can lay out: every control at its own width,
    the name at its floor, and the gaps between them. Below this the parts do
    not fit and no arrangement exists, which is a fact about the widths, not a
    choice. The status block is NOT in this sum: it is the one part that can be
    dropped.
*/
inline constexpr int kRefBarMinW =
      kRefBarPad * 2
    + kRefBarBtnW  + kRefBarGap
    + kRefBarBtnW  + kRefBarGap
    + kRefBarPlayW + kRefBarGap
    + kRefBarNameMinW + kRefBarGap
    + kRefBarBrowseW + kRefBarGap
    + kRefBarAddW;

/** And the narrowest that can also carry the status block. */
inline constexpr int kRefBarMinWithStatusW = kRefBarMinW + kRefBarStatusW + kRefBarGap;

/** Every rect on the bar, from the bar's own bounds.

    THE RIGHT-ANCHORED GROUP IS BOUNDED BY THE LEFT ONE. The first version of
    this function placed Browse and Add from the right edge and the status
    block left of them, with no reference to where the buttons ended. At 300px
    that put the status at x = -54, off the front of the bar and across prev,
    next and play. rf PIN2 caught it before it was ever drawn, which is the
    entire reason this geometry is a pure function.

    hasStatus is a REQUEST, not a guarantee: the block is carried only when the
    bar is at least kRefBarMinWithStatusW, because a status laid over the
    transport is worse than one not shown. In the shipping plugin it is always
    shown, and that is asserted rather than assumed: the narrowest Compare
    column is 585, so the narrowest bar is 565, which is comfortably above the
    508 this needs. The degradation exists so the function is total, not
    because the app can reach it.

    The name is the only flexible region. Below kRefBarMinW the parts do not
    fit at all; the name stays at its floor and the right group stops receding,
    so controls run past the bar's end rather than shrinking to an unhittable
    size or landing on each other.
*/
inline RefBarRects refBarLayout (juce::Rectangle<int> bar, bool hasStatus)
{
    RefBarRects r;
    r.bar = bar;

    auto row = bar.reduced (kRefBarPad, 0);
    const int y = bar.getY() + (bar.getHeight() - 22) / 2;
    auto place = [&] (int& x, int w) { juce::Rectangle<int> q (x, y, w, 22); x += w + kRefBarGap; return q; };

    int x = row.getX();
    r.prev = place (x, kRefBarBtnW);
    r.next = place (x, kRefBarBtnW);
    r.play = place (x, kRefBarPlayW);

    // The floor the right group may never cross: the end of the buttons plus
    // the name's minimum plus its gap.
    const int nameLeft  = x;
    const int rightFloor = nameLeft + kRefBarNameMinW + kRefBarGap;

    const bool carryStatus = hasStatus && bar.getWidth() >= kRefBarMinWithStatusW;

    // THE RIGHT GROUP IS PLACED AS ONE BLOCK, not as three independently
    // clamped rects. Clamping them one at a time was wrong in a way that
    // survived the first fix: at 100px both Browse and Add floored to the SAME
    // x and lay on top of each other. Laying the block once and filling it
    // left to right makes overlap impossible at any width, which is what lets
    // rf PIN2 be a total property rather than one true above a threshold.
    const int blockW = (carryStatus ? kRefBarStatusW + kRefBarGap : 0)
                     + kRefBarBrowseW + kRefBarGap + kRefBarAddW;
    const int blockX = juce::jmax (rightFloor, row.getRight() - blockW);

    int bx = blockX;
    if (carryStatus) { r.status = { bx, y, kRefBarStatusW, 22 }; bx += kRefBarStatusW + kRefBarGap; }
    else             { r.status = {}; }
    r.browse = { bx, y, kRefBarBrowseW, 22 }; bx += kRefBarBrowseW + kRefBarGap;
    r.add    = { bx, y, kRefBarAddW, 22 };

    r.name = { nameLeft, y,
               juce::jmax (kRefBarNameMinW, blockX - kRefBarGap - nameLeft), 22 };
    return r;
}

/** WHICH SLOT THE BAR DRIVES.

    compareTop_ defaults to Live signal and compareBot_ to Empty, so B is the
    slot a reference belongs in and the bar drives it. A carries a reference
    only when the user deliberately put one there, and then the bar follows,
    because the alternative is a bar that names one reference and loads another.

    Takes plain bools rather than CompareSlotState so the rule can be exercised
    without constructing an editor.
*/
inline bool refBarDrivesTopSlot (bool topIsReference, bool botIsReference)
{
    return topIsReference && ! botIsReference;
}

/** PREV AND NEXT, WRAPPING.

    An empty library steps nowhere. From no selection, next lands on the first
    and prev on the last, so both arrows do something on the first press rather
    than one of them appearing broken. Wrapping matches the picture and means
    neither arrow is ever dead, which is the rule the last five commits have
    been about.
*/
inline int refBarStep (int current, int count, int delta)
{
    if (count <= 0) return -1;
    if (current < 0 || current >= count)
        return delta >= 0 ? 0 : count - 1;
    return ((current + delta) % count + count) % count;
}

// ---------------------------------------------------------------------------
// THE SUB-TAB ROW inside REFERENCE.
//
// Compare and Playback. NOT Match: it has no screen yet, and a dead sub-tab is
// the same defect as a dead arrow, which is what the last six commits have
// been about. It joins when it does something.
//
// The row sits BELOW the reference bar, because the selected reference is
// shared by all three surfaces: it belongs to the section, not to one sub-tab.
// ---------------------------------------------------------------------------
enum class RefSubTab { Compare = 0, Playback = 1 };

inline constexpr int kRefSubTabCount = 2;
inline constexpr int kRefSubTabH     = 24;
inline constexpr int kRefSubTabBandH = kRefSubTabH + 4;
inline constexpr int kRefSubTabW     = 96;
inline constexpr int kRefSubTabGap   = 4;

inline const char* refSubTabName (int i)
{
    return i == (int) RefSubTab::Playback ? "PLAYBACK" : "COMPARE";
}

struct RefSubTabRects
{
    juce::Rectangle<int> row;
    juce::Rectangle<int> tab[kRefSubTabCount];
};

/** Left-aligned fixed-width tabs, NOT the top strip's divide-the-width rule.

    computeTabRects splits the window between eight tabs because the strip owns
    the full width. This row does not: it sits above content and two tabs
    stretched across 1380px would read as a header, not as a choice. Fixed
    width, left aligned, and the row keeps its full width so the band height is
    the same whatever is in it.
*/
inline RefSubTabRects refSubTabLayout (juce::Rectangle<int> row)
{
    RefSubTabRects r;
    r.row = row;
    for (int i = 0; i < kRefSubTabCount; ++i)
        r.tab[i] = { row.getX() + i * (kRefSubTabW + kRefSubTabGap), row.getY(),
                     kRefSubTabW, kRefSubTabH };
    return r;
}

/** Which sub-tab contains p, or -1. Pure, so the hit test and the painting
    cannot disagree about where a tab is. */
inline int refSubTabAt (const RefSubTabRects& r, juce::Point<int> p)
{
    for (int i = 0; i < kRefSubTabCount; ++i)
        if (r.tab[i].contains (p)) return i;
    return -1;
}

} // namespace echojay
