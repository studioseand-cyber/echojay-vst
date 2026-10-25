#pragma once
// =============================================================================================
//  EJEngageDemand.h - WHEN THE V2'S OWN WINDOW IS A DEMAND FOR THE RACK IT IS SHOWING
//  (his 06:55 fault, 25 Oct 2026)
// =============================================================================================
//  Sean, after the morning tests passed:
//
//      *"after closing the Link's window and opening the V2, the rack strip stayed greyed with
//      "Not connected to this rack." ... It only reconnected after I selected another Link's rack
//      and came back. ... It must reconnect by itself."*
//
//  ---- WHAT THE LOG SAYS, AND WHY SWITCHING RACKS CURED IT -------------------------------------
//
//      06:54:25  EJRackLock: released uid=4814a16004          (his V2's window went)
//      06:55:15  EJBorrow: not engaging 4814a16004 - nothing has asked to engage this rack yet
//      06:55:32  EJBorrow: not engaging 4814a16004 - nothing has asked to engage this rack yet
//      06:56:06  EJRackSel: row=28 ... -> viewing it           (he switched away)
//      06:56:12  EJRackSel: row=29 ... -> viewing it           (and back: ENGAGED, rev 1)
//
//  The engage is armed in ONE place, `handleBorrowSelectionChange`, and only for a selection that
//  CHANGES - `decideSelection` returns Nothing/ViewOnly otherwise and the function returns before
//  `pendingAutoEngage_` is set. A rack is selected once per PROCESSOR; an editor is built many
//  times over that processor's life. So the first window to open on a rack engages it and every
//  later one inherits the view with no session, which is the state he was looking at: the strip
//  greyed, "Not connected to this rack.", and the sticky banner from the last apply still in the
//  middle of the page because the banner is cleared ON ENGAGE.
//
//  Switching racks cured it because that IS a selection change - twice - so the second one armed
//  the engage the reopen never did.
//
//  ---- WHAT THIS DOES NOT UNDO ----------------------------------------------------------------
//
//  His 20 Oct ruling stands: *"Viewing a rack must not engage the borrow/edit session."* That
//  ruling is about the TICK, which armed an engage from `chainViewUid()` every second and produced
//  a family of staleness while he sat in the chat with a rack merely in view. This is not the tick
//  re-arming: it is ONE ask, when a window OPENS on a Link's rack, which is exactly what the first
//  window on that rack has always done. The inconsistency was the bug - the same gesture (open the
//  V2 on a Link's rack) connected or did not depending on whether some earlier editor had already
//  selected it.
//
//  ONCE PER WINDOW PER RACK. The caller holds the "already asked" fact, so a Link that is holding
//  its own rack is not asked once a second for ever: the ask stands as `pendingAutoEngage_` and the
//  tick waits on the lock, which is what hands control straight back when the Link's window closes
//  (his 08:44 ruling of 23 Oct).
// =============================================================================================

#include <JuceHeader.h>

namespace echojay
{

/** The decision, with the reason it was taken - the reason is logged, so a refusal can be read
    rather than guessed at. `why` is a sentence fragment that follows the rack's uid. */
struct EngageAsk
{
    bool ask = false;
    const char* why = "";
};

/** Should this window ask to engage the rack it is showing? Every input is a fact the caller
    already has; nothing here reads the world, so the guard can hold both directions. */
inline EngageAsk windowOpenEngageAsk (bool viewIsALinkRack,
                                      bool sessionActive,
                                      bool somethingPending,
                                      bool readInFlight,
                                      bool applyInFlight,
                                      bool linkPublishesAnEditableRack,
                                      bool alreadyAskedForThisRack)
{
    if (! viewIsALinkRack)   return { false, "the view is not a Link's rack" };
    if (sessionActive)       return { false, "this rack is already engaged" };
    if (somethingPending)    return { false, "an engage is already pending" };
    if (readInFlight)        return { false, "a rack read is in flight" };
    if (applyInFlight)       return { false, "an apply is in flight" };
    if (! linkPublishesAnEditableRack)
        return { false, "this Link has not published a rack this build can edit yet" };
    if (alreadyAskedForThisRack)
        return { false, "this window has already asked for this rack" };
    return { true, "a window is open on this Link's rack with no session - ASKING TO ENGAGE, once "
                   "for this window, because opening the V2 on a rack is what connects to it (his "
                   "06:55 fault: the first window to open engaged it and every later one did not)" };
}

} // namespace echojay
