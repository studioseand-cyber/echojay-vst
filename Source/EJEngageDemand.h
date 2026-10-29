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
//  ONCE PER WINDOW PER RACK. The caller holds the "has already had it" fact, so a Link that is
//  holding its own rack is not asked once a second for ever: the ask stands as `pendingAutoEngage_`
//  and the tick waits on the lock, which is what hands control straight back when the Link's window
//  closes (his 08:44 ruling of 23 Oct).
//
//  ---- AND "HAD IT" IS WIDER THAN "ASKED FOR IT" (25 Oct 2026) --------------------------------
//
//  The caller's fact was first written as "this window has already ASKED", and that was too narrow
//  to be safe. A session can arrive by another road - the X press, a plugin-parameter dial, the
//  agent's run - and a window that never asked had its ask still in hand. So when such a session
//  was RELEASED (a lease loss, or the close-apply handing the rack back, which is the "owns its
//  rack again" screen) the very next tick asked and re-engaged, taking the rack back on its own.
//  overlay_liveness_guard's leg b2 is exactly that case and went red on it.
//
//  So the fact is ONE CONNECTION PER WINDOW PER RACK, however the connection was made. A released
//  session stays released and the strip says so honestly, which is his 20 Oct ruling; a fresh
//  window still connects once, which is his 06:55 one.
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
                                      const juce::String& viewUid,
                                      const juce::String& sessionUid,
                                      bool somethingPending,
                                      bool readInFlight,
                                      bool applyInFlight,
                                      bool linkPublishesAnEditableRack,
                                      bool windowHasAlreadyHadThisRack)
{
    // ---- THE TWO UIDs, NOT A BOOL (his 21:00 fault, 28 Oct 2026) -----------------------------
    //
    // This took `sessionActive` and read it as "THIS rack is already engaged". The call site had
    // only a global fact to give it - borrowActive() - which is "SOME rack is engaged", and the
    // two differ exactly when a session is live on one rack and the window is looking at another.
    // His 21:00, with the uids from his own log:
    //
    //   21:00:33.490  session ACTIVE <- true (engage begin, uid=4814a16004)
    //   21:00:37.055  EJRackSel: row=3 resolved=bb411b8240 -> viewing it   (the LOCK moves...
    //   21:00:37.058  EJRackLock: acquired uid=bb411b8240                   ...the SESSION does not)
    //   21:00:37 - 21:01:45   lock held for bb411b8240, session still on 4814a16004:
    //                         this function answered "this rack is already engaged" about a rack
    //                         that was not engaged, so nothing asked, pendingAutoEngage_ stayed
    //                         empty, and the rack area sat on "Getting this rack ready to edit..."
    //   21:01:45      he closed the window (the stale session released with it)
    //   21:01:50.564  session ACTIVE <- true (engage begin, uid=bb411b8240)  - the reopen connected
    //
    // Reopening "fixed" it only because closing released the session that was blocking the ask.
    //
    // SO THE MISTAKE IS NOT EXPRESSIBLE ANY MORE: the uids come in and the comparison is made
    // here. A bool cannot be passed by accident, and a caller that has no session passes an empty
    // string, which is the honest shape of "nothing is engaged".
    if (! viewIsALinkRack)   return { false, "the view is not a Link's rack" };
    if (sessionUid.isNotEmpty() && sessionUid == viewUid)
                             return { false, "this rack is already engaged" };
    if (somethingPending)    return { false, "an engage is already pending" };
    if (readInFlight)        return { false, "a rack read is in flight" };
    if (applyInFlight)       return { false, "an apply is in flight" };
    if (! linkPublishesAnEditableRack)
        return { false, "this Link has not published a rack this build can edit yet" };
    if (windowHasAlreadyHadThisRack)
        return { false, "this window has already had this rack - one connection per window per "
                        "rack, so a released session is not taken back by itself (his 20 Oct "
                        "ruling: viewing is not asking)" };
    // A SESSION ON ANOTHER RACK DOES NOT BLOCK THIS ONE'S ASK. It is not this window's doing that
    // one is live elsewhere - a row click moves the view and leaves the session where it is, which
    // is his 20 Oct ruling working as written - and a window that opens on a rack has always been
    // allowed its one ask. Whether the OTHER session should end is the selection road's business
    // (decideSelection: a USER selection applies and moves it), never this function's.
    return { true, "a window is open on this Link's rack with no session - ASKING TO ENGAGE, once "
                   "for this window, because opening the V2 on a rack is what connects to it (his "
                   "06:55 fault: the first window to open engaged it and every later one did not)" };
}

/** Has this WINDOW already had this rack - asked for it, held a session on it, or been opened
    while one was live? The one input `windowOpenEngageAsk` cannot work out for itself.

    A COUNT, not a flag. The first cut of this read `borrowActive()` on the tick, which can only
    see a session while it is still alive: overlay_liveness_guard's b2 engages on an X press and
    releases before the next tick, so nothing ever observed the session and the window's ask was
    still in hand when it had come and gone - the tick then took the rack back by itself. A window
    that records the rack's session COUNT when it opens needs no observation and no timing.

    `thisRackWasLiveWhenWindowOpened` is the other end of the same question: the V2 reopened in the
    middle of a live session has had that rack too, and its count was already incremented before
    the window existed. */
inline bool windowHasHadRack (int sessionsStartedNow,
                              int sessionsStartedWhenWindowOpened,
                              bool thisRackWasLiveWhenWindowOpened,
                              bool thisWindowAlreadyAsked)
{
    if (thisRackWasLiveWhenWindowOpened) return true;
    if (thisWindowAlreadyAsked)          return true;
    return sessionsStartedNow > sessionsStartedWhenWindowOpened;
}

} // namespace echojay
