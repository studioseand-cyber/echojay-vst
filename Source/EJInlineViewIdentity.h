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
};

/** Does the panel have to open the selected slot's editor? */
inline bool inlineNeedsReopen (const InlineViewState& s, int selectedSlot,
                               const juce::String& viewRackUid) noexcept
{
    if (selectedSlot < 0) return false;                       // nothing is selected: nothing to open
    const bool popoutCoversIt = s.havePopout && s.popoutSlot == selectedSlot
                             && s.popoutRackUid == viewRackUid;
    if (popoutCoversIt) return false;                         // it is already up, in its own window
    const bool inlineCoversIt = s.haveInlineEditor && s.inlineSlot == selectedSlot
                             && s.inlineRackUid == viewRackUid;
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
inline bool mayTakeLentEditor (bool deliberate, bool revokeStandsHere) noexcept
{
    return deliberate || ! revokeStandsHere;
}

} // namespace echojay
