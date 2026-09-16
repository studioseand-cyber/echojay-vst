#pragma once

#include <JuceHeader.h>

// WHERE A TOOLTIP GOES, as a pure function.
//
// THE INTERFACE IS THE CONSTRAINT, and it is worth stating because it bounds
// what any placement rule here can possibly do. LookAndFeel::getTooltipBounds
// receives the tip's TEXT, one POINT, and one RECTANGLE. That is all.
// TooltipWindow resolves the component under the mouse in its timerCallback and
// keeps it private (lastComponentUnderMouse), passing only the text onward
// through updatePosition, so the bounds of the control the tip DESCRIBES are
// not available at this layer and cannot be obtained from it. The cursor is the
// only anchor there is. A rule that tries to avoid covering "the control" is
// therefore not implementable here; a rule that keeps the tip inside the parent
// is.
//
// THE POINT IS PARENT-LOCAL, NOT A SCREEN POSITION, whenever the window has a
// parent, which for a plug-in it always should:
//
//     updatePosition (tip, parent->getLocalPoint (nullptr, screenPos),
//                     parent->getLocalBounds());          juce_TooltipWindow.cpp:123
//
// JUCE's own parameter is named screenPos and is not one. parentArea is
// origin-zero and sized to the parent, which is why a tip served by a window
// parented to a PANEL is positioned against the panel's origin and clamped to
// the panel's bounds rather than the editor's.
//
// WHAT THIS REPLACES. The rule was "which half of the window is the cursor in":
//
//     screenPos.x > parentArea.getCentreX() ? left : right
//     screenPos.y > parentArea.getCentreY() ? above : below
//
// A centre line is a proxy for "is there room", and it is wrong in both
// directions. A cursor one pixel past the centre flipped the tip above even
// with four hundred pixels of space below it, and a cursor just before the
// centre placed a tall tip below with nowhere for it to go, leaving the clamp
// to drag it back up. It also made placement depend on the size of the PARENT
// rather than on the size of the TIP, so the same control gave different
// answers in a panel and in the editor.
//
// THE RULE NOW IS FIT, NOT POSITION. Prefer below, and go above only when below
// would not fit inside parentArea. Prefer right, and go left only when right
// would not fit. A clamp still runs afterwards in getTooltipBounds as a final
// guarantee, but it is NOT what makes the decision: constrainedWithin slides a
// rectangle until it is inside, and sliding cannot re-choose a side. A tip that
// should have gone above arrives below, overlapping what it describes, and the
// clamp reports success.
//
// THE OFFSETS ARE THE ONES THAT WERE THERE. Nothing about the gaps is being
// changed here, only the choice of side, so a tip that was already correctly
// placed lands in exactly the same pixel.
namespace echojay {

constexpr int kTipGapBelow = 20;   // clears a normal cursor hotspot
constexpr int kTipGapAbove = 4;
constexpr int kTipGapRight = 16;
constexpr int kTipGapLeft  = 10;

/** The tooltip's top-left corner, given the cursor, the tip's size and the
    area the tip must stay inside. All parent-local. No LookAndFeel state and
    no component access, so a pin can exercise every case with no window.

    A TIP BIGGER THAN parentArea still gets an answer rather than an assertion:
    neither side fits, so both flip, and the caller's clamp pulls the result
    back inside. That is the same pixel the un-flipped version would have been
    clamped to, so the degenerate case costs nothing and needs no special
    branch here.
*/
inline juce::Point<int> tooltipOrigin (juce::Point<int> mouse,
                                       int tipW, int tipH,
                                       juce::Rectangle<int> parentArea)
{
    const int below = mouse.y + kTipGapBelow;
    const int right = mouse.x + kTipGapRight;

    const int y = (below + tipH <= parentArea.getBottom())
                      ? below
                      : mouse.y - (tipH + kTipGapAbove);

    const int x = (right + tipW <= parentArea.getRight())
                      ? right
                      : mouse.x - (tipW + kTipGapLeft);

    return { x, y };
}

} // namespace echojay
