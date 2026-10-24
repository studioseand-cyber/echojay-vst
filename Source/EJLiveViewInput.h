#pragma once
//  EJLiveViewInput.h — WHETHER THE LIVE VIEW TAKES A GESTURE, AND WHAT IT SAYS WHEN IT DOES NOT
//
//  HIS RULING, 24 Oct 2026, after the first live view came up clean:
//
//      "the copy accepts knob changes, and they go nowhere: the audio didn't change, and when I
//       closed it and opened the Link, the Link's CL 1B didn't have my settings. Silent lost
//       changes are a fault. 1. Now, small: until control exists, the view blocks input and says
//       so on the view ("View only - change settings on the Link for now")."
//
//  WHY THE OLD ATTEMPT DID NOT HOLD. EJShadowView::createEditorIfPossible called
//  setInterceptsMouseClicks (false, false) on the editor component, and its own note said why that
//  is only half a block: *"this is JUCE's own interception, so it covers a JUCE-drawn editor
//  completely. A guest AU that draws its own NSView may still receive clicks on that view."*  The
//  Tube-Tech is exactly that - CSoftubeAUInterfaceCocoaView, a real NSView reparented into our
//  clip container - so JUCE never saw the clicks to drop them. Blocking it for real is an AppKit
//  fact: a transparent NSView of our own, above the plugin's, that swallows the events
//  (NativeClip2_setShield).
//
//  THE DECISION ITSELF IS HERE, in a function with no state and no window in it, so the gate can
//  ask it what it answers in each case. The SHIELD is native and cannot be unit-tested headlessly;
//  that half is checked by hand on the installed build and the check is named in the round's notes
//  rather than implied.
#include <JuceHeader.h>
#include "EJLiveControl.h"   // the refusal's words live with CONTROL, not in a second copy here

namespace echojay
{
/** Is control through the live view BUILT? Stage C/D of the muted-shadow design - a gesture on the
    V2's copy travelling to the real plugin on the Link, and the view following changes made there.
    Designed (docs/LIVE_VIEW_CONTROL_2026-10-24.md), not built, so this is false and the view is
    read-only. When it goes true the shield goes with it, and this is the one line to change. */
// ---- AND NOW IT IS BUILT (24 Oct 2026, after FOLLOW layer 2 passed at 21:31) ----------------
//
// *"turning a knob in the V2's live view changes the real plugin on the Link at once (I hear it,
// the Link keeps it), and the "View only" lock comes off for parameters."*
//
// The flag stays, and it stays the ONE place the shield's decision is read from, because the lock
// coming off is not the end of the subject: the shield goes straight back up, with a different
// sentence, whenever a gesture cannot actually travel (an agent run holding the rack, or a view
// that is not following). One switch, two reasons, one function.
// ---- AND BACK OFF, BY HIS RULING OF 22:1x (24 Oct 2026) -------------------------------------
//
// *"Until then, put the "View only" lock back on in any build you ship."*
//
// CONTROL WORKED - his gestures reached the real plugin and the Link held them (the log says
// `slot 1 "Threshold" set to 0.3650 ("-11.7") ... this Link now holds it`). What went wrong was
// downstream and older: the borrow session's close-apply committed its UNSEEDED mirror's default
// state over the Link when the view came down, so the setting he had just heard was wiped. Until
// that road cannot write a state it has no claim to, a gesture through the view is a change that
// may be silently undone - which is the same fault as a change that never arrives.
//
// ONE LINE, AND IT IS THE ONLY LINE NEEDED, because the shield's decision has one owner.
constexpr bool kLiveViewControlBuilt = false;

/** What the view says while it is read-only. His words, kept verbatim. */
inline const char* liveViewOnlyLine() noexcept
{ return "View only - change settings on the Link for now"; }

/** What the view does about gestures. */
struct LiveViewInputPolicy
{
    bool         blockInput = false;   ///< put the shield up and swallow every gesture
    juce::String line;                 ///< what to draw on the view; empty when nothing is said
};

/** ONE RULE, ASKED IN ONE PLACE.

    A copy that CANNOT pass a gesture on must not accept one, because a knob that moves and changes
    nothing is a silent lost change - his words, and the house's own fault family (a picture
    disagreeing with the thing it is a picture of). So:

      - a live view, with control not built  -> blocked, and it says so;
      - a live view, with control built, but an agent run holding that rack, or the view not
        following the plugin -> blocked, with THAT reason, because a gesture that cannot land is
        the same silent loss whatever is stopping it (24 Oct 2026);
      - a live view, with control built and able to travel -> open, and it says nothing;
      - anything that is NOT a live view      -> untouched. The main rack's own slots, a borrowed
        mirror and an edit session all host the instance whose settings a gesture really changes,
        and none of them has ever needed a shield. */
inline LiveViewInputPolicy liveViewInputPolicy (bool editorIsALiveViewCopy,
                                                bool controlBuilt = kLiveViewControlBuilt,
                                                bool following = true,
                                                bool runHoldsRack = false)
{
    if (! editorIsALiveViewCopy) return { false, {} };
    if (! controlBuilt)          return { true, juce::String (liveViewOnlyLine()) };
    // ---- CONTROL IS BUILT, SO THE QUESTION IS WHETHER IT MAY TRAVEL NOW (24 Oct 2026) ---------
    // The shield is the right surface for both refusals, and it already does exactly the right
    // thing: it swallows the gesture and draws the reason over the plugin's own view. What it must
    // NOT do is invent a second rule - so the decision is liveControlPolicy's, and this renders it.
    const auto control = liveControlPolicy (following, runHoldsRack);
    if (! control.allow) return { true, control.line };
    return { false, {} };
}
} // namespace echojay
