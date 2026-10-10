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
// Compare, Match and Playback, IN THAT ORDER, which MATCH_REFERENCE_PLAN 8A.1
// decided: Compare describes the delta, Match proposes moves against it,
// Playback plays the user's audio through a simulated end. Match sits between
// the two because it is the step between describing and listening, not an
// extra appended after them.
//
// Match joined on 19 Sep 2026 with a page that says what it is for. It was kept
// out until then because a dead sub-tab is the same defect as a dead arrow.
//
// The row sits BELOW the reference bar, because the selected reference is
// shared by all three surfaces: it belongs to the section, not to one sub-tab.
// ---------------------------------------------------------------------------
//
// ENUM VALUE == POSITION IN THE ROW. The paint highlight compares the loop
// index with (int) refSubTab_, and mouseDown casts the hit index back to the
// enum, so the order here IS the order on screen.
//
// Count IS A SENTINEL, NEVER A TAB. It is last so that it equals the number of
// real values, and kRefSubTabCount is taken from it rather than written as a
// second literal that could disagree with the enum.
enum class RefSubTab { Compare = 0, Match = 1, Playback = 2, Count };

inline constexpr int kRefSubTabCount = (int) RefSubTab::Count;
inline constexpr int kRefSubTabH     = 24;
inline constexpr int kRefSubTabBandH = kRefSubTabH + 4;
inline constexpr int kRefSubTabW     = 96;
inline constexpr int kRefSubTabGap   = 4;

/** True for a position that names a real sub-tab: 0 up to, not including,
    Count. The one test every index-to-enum conversion goes through. */
inline constexpr bool refSubTabIndexValid (int i) noexcept
{
    return i >= 0 && i < kRefSubTabCount;
}

/** The painted names, BY POSITION. The static_assert ties the table to the
    enum, so a value added before Count without a name here does not compile,
    rather than painting a blank or a neighbour's name. */
inline constexpr const char* kRefSubTabNames[] = { "COMPARE", "MATCH", "PLAYBACK" };
static_assert (sizeof (kRefSubTabNames) / sizeof (kRefSubTabNames[0]) == (size_t) kRefSubTabCount,
               "every RefSubTab before Count needs exactly one name in kRefSubTabNames");

/** What an out-of-range position is called. Deliberately wrong-looking: the
    ternary this replaces answered "COMPARE" for every position that was not
    Playback, so a bad index painted as a plausible tab. This cannot be taken
    for one. */
inline constexpr const char* kRefSubTabInvalidName = "NO SUCH TAB";

inline const char* refSubTabName (int i)
{
    if (! refSubTabIndexValid (i))
        return kRefSubTabInvalidName;
    return kRefSubTabNames[i];
}

/** WHETHER THE COMPARE SUB-TAB'S TEN CONTROLS SHOW: the meter row, the two
    slot buttons, the two play buttons, A, B, the shared play, sync and AI
    Compare. The editor's showCompareFurniture applies it and nothing else
    decides it.

    TWO INPUTS, NOT ONE. The sub-tab alone cannot be the rule: leaving the
    Reference tab resets the sub-tab to Compare AND hides the controls, so
    "Compare" is true both while they show and while they must not. Whether the
    Compare view is up at all is the second input.

    Before this there were two authors that disagreed. setRefSubTab derived
    the value from the sub-tab alone, and showCompareView passed true
    unconditionally, justified by a comment saying the sub-tab was always
    Compare on entry. showCompareView is also a refresh called from Playback
    (a file drop, removing a reference, rename and delete), so it showed the
    controls over the Playback page. */
inline constexpr bool compareFurnitureVisible (bool compareVisible, RefSubTab t) noexcept
{
    return compareVisible && t == RefSubTab::Compare;
}

struct RefSubTabRects
{
    juce::Rectangle<int> row;
    juce::Rectangle<int> tab[kRefSubTabCount];
};

// ---------------------------------------------------------------------------
// ONE CELL RULE, SHARED BY EVERY ROW OF EQUAL CELLS
// ---------------------------------------------------------------------------
//
// THIS LIVES HERE BECAUSE THIS IS THE LOWEST HEADER THAT NEEDS IT. The Match
// page's axis row reads the same two constants and calls the same function
// (EJMatchPage.h includes this file for them), so the sub-tab row and the axis
// row cannot drift apart: change the gap here and both rows change together.
// The alternative was two literals in two files that happened to agree today.
inline constexpr int   kEjCellGap    = 6;
inline constexpr float kEjCellRadius = 6.0f;

/** One cell of `count`, evenly across the row with kEjCellGap between them.
    Integer division leaves the remainder on the LAST cell rather than in a gap
    that drifts, so the row always reaches its right edge exactly. */
inline juce::Rectangle<int> ejEvenCell (juce::Rectangle<int> row, int i, int count,
                                        int gap = kEjCellGap)
{
    if (i < 0 || i >= count || count <= 0 || row.getWidth() <= 0) return {};
    const int gaps  = gap * (count - 1);
    const int cellW = juce::jmax (0, (row.getWidth() - gaps) / count);
    const int x     = row.getX() + i * (cellW + gap);
    const int w     = (i == count - 1) ? juce::jmax (0, row.getRight() - x) : cellW;
    return { x, row.getY(), w, row.getHeight() };
}

/** THE THREE SUB-TABS, SPANNING THE ROW AS EQUAL COLUMNS.

    THIS REVERSES A DELETED DECISION AND THE OLD REASONING IS KEPT SO THE
    REVERSAL IS VISIBLE. It used to read: left aligned and FIXED at
    kRefSubTabW, "NOT the top strip's divide-the-width rule", because "two tabs
    stretched across 1380px would read as a header, not as a choice".

    WHAT CHANGED IS THAT THE ROW GAINED A FAMILY. When that was written the row
    was three small buttons above a page with nothing like them. The Match
    page's axis row is now four equal cells spanning its card with the same gap
    and the same corner, and the sub-tab row sitting as three narrow buttons at
    the left read as a different kind of control rather than the same kind one
    level up. Sharing ejEvenCell makes them one family.

    THE OLD WORRY IS REAL AND IS NOT DISMISSED: at a very wide window three
    cells across the whole card do read more like a header. It is accepted
    because the axis row already does exactly that and is the look being
    matched, and because the row's own selected fill still marks it as a
    choice. If it turns out wrong at 1800px the fix is a maximum cell width
    here, in one place, for both rows. */
inline RefSubTabRects refSubTabLayout (juce::Rectangle<int> row)
{
    RefSubTabRects r;
    r.row = row;
    for (int i = 0; i < kRefSubTabCount; ++i)
        r.tab[i] = ejEvenCell (row, i, kRefSubTabCount).withHeight (kRefSubTabH);
    return r;
}

/** Which sub-tab contains p, or -1. Pure, so the hit test and the painting
    cannot disagree about where a tab is.

    THE LOOP BOUND IS THE GUARANTEE. i runs from 0 to kRefSubTabCount - 1, so
    every index returned here is a real tab at any count, and there is nothing
    for a check inside the loop to catch. This returns an int, not an enum.
    The place an index BECOMES a RefSubTab is the cast in mouseDown
    (PluginEditor.cpp), and that is where the real guard lives:
    refSubTabIndexValid, pinned by rs PIN3. */
inline int refSubTabAt (const RefSubTabRects& r, juce::Point<int> p)
{
    for (int i = 0; i < kRefSubTabCount; ++i)
        if (r.tab[i].contains (p)) return i;
    return -1;
}

/** Whether p is on the sub-tab row at all: the row's band, or any tab,
    including a tab a narrow row does not fully contain.

    A press here is navigation or nothing. It never belongs to whatever the row
    sits above, and that is the whole use of this: the editor's right-click and
    double-click handlers consume a press here instead of letting it fall
    through to Compare's rename and delete. Pure, and from the same rects the
    paint draws, so the two cannot disagree about where the row is. */
inline bool refSubTabRowHit (const RefSubTabRects& r, juce::Point<int> p)
{
    return r.row.contains (p) || refSubTabAt (r, p) >= 0;
}

} // namespace echojay
