#pragma once
#include <JuceHeader.h>

/** WHAT IS ON SCREEN IS A RACK AND A SLOT, NEVER A SLOT ALONE (27 Oct 2026).

    The panel re-opens the slot editor from its refresh when what is open is not what is selected.
    That test used to be `inlineSlot != selectedIdx`, and a slot index does not say which rack it
    belonged to - so moving the view from one rack to another and selecting the SAME index on the new
    one re-opened nothing, and the pane went on showing the old rack's editor behind the new rack's
    cards.

    It cost RH3 ("switching between slots") a red overnight and eighteen seconds of a rig pass: the
    row before it had left the V2's own slot 1 open, RH3 aimed at a Link and double-clicked its slot
    1, and the pane held the own rack's copy of AUMultibandCompressor the whole time. The row read
    "no lent editor, the road is copy" and was telling the truth about the product.

    This is the same mistake as "a slot index is not a plugin" (660d1f95) one floor up, and the
    decision is pulled out here as a pure function for the same reason realEditorRoad is: so a guard
    can drive it exactly, in both directions, instead of a UI test trying to.

    uid conventions: empty = the V2's own rack. A popout counts as "showing that slot" only when it
    belongs to the rack in view, for exactly the same reason. */
namespace echojay
{

struct InlineViewState
{
    int  inlineSlot      = -1;      ///< the slot the inline editor is open on, -1 for none
    bool haveInlineEditor = false;  ///< ...and whether there is actually an editor in it
    juce::String inlineRackUid;     ///< the rack that editor belongs to
    bool havePopout      = false;
    int  popoutSlot      = -1;
    juce::String popoutRackUid;
    /** WHICH ROAD the inline editor is on: the Link's own instance (true) or this window's copy of
        it (false). A copy and the real thing are both "an editor for that slot of that rack", and
        for one question - may a press replace what is there? - they are not interchangeable. */
    bool inlineIsLent    = false;
};

/** Does the panel have to open the selected slot's editor?

    @param wantLent  TRUE when the caller is asking for the LINK'S OWN editor specifically - a
                     deliberate press on a slot whose real editor is available here. A copy of that
                     plugin then does NOT count as covering it, because the press is a request for
                     the real one and showing a copy instead silently answers a different question.

    AN INLINE EDITOR IS NOT A ROAD (27 Oct 2026, the RE15 red). This is the same shape as the rack
    fault above, one floor along. After the Link took its editor back, the automatic path correctly
    refused to grab it again and fell through to the copy road - so a copy of the plugin was on
    screen for that slot of that rack. Every term of the old test was then satisfied, the press
    answered "leave it alone", and the V2 could never get the real editor back: RE15 went 0 of 3
    while RE14 went 3 of 3, which is one bug wearing two faces. */
inline bool inlineNeedsReopen (const InlineViewState& s, int selectedSlot,
                               const juce::String& viewRackUid, bool wantLent = false) noexcept
{
    if (selectedSlot < 0) return false;                       // nothing is selected: nothing to open
    const bool popoutCoversIt = s.havePopout && s.popoutSlot == selectedSlot
                             && s.popoutRackUid == viewRackUid
                             && (! wantLent || s.inlineIsLent);
    if (popoutCoversIt) return false;                         // it is already up, in its own window
    const bool inlineCoversIt = s.haveInlineEditor && s.inlineSlot == selectedSlot
                             && s.inlineRackUid == viewRackUid
                             && (! wantLent || s.inlineIsLent);
    return ! inlineCoversIt;
}

/** MAY THIS OPEN ASK FOR THE LINK'S OWN EDITOR? (27 Oct 2026, RE14)

    `inlineNeedsReopen` above says the pane has nothing on screen for the selected slot. It does not
    say the pane may go and TAKE the Link's instance - and after the user has opened the plugin on
    the Link's own window, it may not. The Link takes its editor back then and this window is told;
    the next settle found the pane empty and asked for the lease again, 180 ms later, so both windows
    believed they had the plugin. That is the state RE14 names and the road both crash reports came
    down: an editor built for a second time on one instance while another window still held the
    first.

    A press is the user asking for it here again, and it clears the suppression before this is asked;
    nothing automatic clears it. So: deliberate opens always may, and an automatic one may only while
    no revoke stands for that rack and slot.

    @param deliberate        his press, the pop-out button, the rig's open_slot - not a refresh tick
    @param revokeStandsHere  the Link took this very slot's editor back and he has not asked again */
/** NOTE (28 Oct 2026, after LV11): the answer gates the WHOLE open, not just the lease road. A
    false here means this window shows nothing for that slot - not a copy either - until a press
    asks. Refusing only the lease let the copy road pick the slot up, and LV11 measured a copy of a
    Link's plugin arriving in the V2's own output (0.0025 against a floor of 0.0000004) where the
    round before had moved it by exactly nothing. */
inline bool mayTakeLentEditor (bool deliberate, bool revokeStandsHere) noexcept
{
    return deliberate || ! revokeStandsHere;
}

} // namespace echojay
