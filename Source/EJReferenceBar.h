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
    // slot is the A/B letter. IT IS A RECT NOW, and it was not: paint drew it
    // from rb.play.getRight() + 2, spanning 108..120 while the name began at
    // 110, so the letter sat on top of the first characters of every reference
    // name. rf PIN2 could not see it because it was not in this struct, which
    // is open list 152's family committed inside the bar that pin guards.
    //
    // scope names the folder the arrows step within. Empty for ALL REFERENCES:
    // that is the default and a chip saying so would be noise on every bar.
    juce::Rectangle<int> bar, prev, next, play, slot, scope, name, status, browse, add;
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
inline constexpr int kRefBarSlotW    = 14;   // the A/B letter
inline constexpr int kRefBarScopeW   = 84;   // the folder chip
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
    + kRefBarSlotW + kRefBarGap
    + kRefBarNameMinW + kRefBarGap
    + kRefBarBrowseW + kRefBarGap
    + kRefBarAddW;

/** And the narrowest that can also carry the status block. */
inline constexpr int kRefBarMinWithStatusW = kRefBarMinW + kRefBarStatusW + kRefBarGap;

/** And with the scope chip on top of that.

    PRIORITY WHEN THE WIDTH WILL NOT TAKE EVERYTHING: name, then status, then
    scope. The status is the only thing that reports a failed drop and it is
    transient; the chip is persistent context and returns the moment the
    message clears. Measured: the narrowest bar the product can produce is 565
    (900px window floor), which carries name + status at 526 and name + scope
    at 430, but not all three at 614. So the chip drops only while a message is
    showing at close to the minimum window, and it comes back by itself. */
inline constexpr int kRefBarMinWithScopeW = kRefBarMinW + kRefBarScopeW + kRefBarGap;
inline constexpr int kRefBarMinWithBothW  = kRefBarMinWithStatusW + kRefBarScopeW + kRefBarGap;

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
inline RefBarRects refBarLayout (juce::Rectangle<int> bar, bool hasStatus, bool hasScope)
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
    r.slot = place (x, kRefBarSlotW);      // the A/B letter, a rect at last

    // THE SCOPE CHIP SITS BESIDE THE SLOT LETTER, at the head of the bar where
    // the arrows are, because it says what the arrows step THROUGH. Dropped
    // before the status is, per the priority above.
    const bool carryStatus = hasStatus && bar.getWidth() >= kRefBarMinWithStatusW;
    const bool carryScope  = hasScope
                          && bar.getWidth() >= (carryStatus ? kRefBarMinWithBothW
                                                            : kRefBarMinWithScopeW);
    if (carryScope) r.scope = place (x, kRefBarScopeW);
    else            r.scope = {};

    // The floor the right group may never cross: the end of the head group
    // plus the name's minimum plus its gap.
    const int nameLeft  = x;
    const int rightFloor = nameLeft + kRefBarNameMinW + kRefBarGap;

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

/** DOES THE BAR NEED LAYING OUT AGAIN?

    refBarLayout takes hasStatus and gives the name region 184px more when there
    is no message, so the ONE thing the bar's geometry depends on is whether a
    status is PRESENT. Not what it says: a message replaced by another message
    changes no rectangle.

    WHY THIS IS A FUNCTION AND NOT TWO INLINE `isNotEmpty()` CALLS. It is the
    rule that couples a setter to a layout, and a rule that lives only inside
    the setter is true on the day it is written and unchecked afterwards. As a
    pure function a pin can state it: transitions relayout, same-presence
    changes do not, and clearing is a transition as much as setting is. The
    relayout ACTUALLY HAPPENING is wiring in the setter and no pin here reaches
    it.

    SAFE TO ACT ON FROM THE SETTER, and that was established before it was
    written rather than assumed: setRefStatus is reached from eight entry points
    (the preset onChange lambda, two preset buttons, filesDropped,
    loadReferenceFile, saveCurrentPreset, loadPreset, refBarStepBy) and
    resized()'s entire 1532-line body calls none of them. resized() only READS
    the label's text. So there is no write-from-layout edge and no re-entrancy.
*/
inline bool refStatusPresenceChanged (const juce::String& before,
                                      const juce::String& after)
{
    return before.isNotEmpty() != after.isNotEmpty();
}

/** ARE THE ARROWS LIVE?

    Pressing an arrow with nothing to step through is not an error, so it must
    not produce a message: the control says it instead. This is the predicate
    that decides, kept beside refBarStep because the two answer the same
    question from opposite sides, and pure so the rule is checkable rather than
    a condition buried in a handler.

    THE COUNT IS THE SCOPE'S, not the library's, for the same reason
    refBarStep's is: a full library and an empty folder must disable the
    arrows exactly alike.
*/
inline bool refBarArrowsEnabled (int scopeCount) { return scopeCount > 0; }

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
