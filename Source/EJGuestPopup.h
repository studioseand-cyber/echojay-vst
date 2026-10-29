#pragma once
// =============================================================================================
//  EJGuestPopup.h - WHAT MUST HAPPEN BEFORE A POPUP IS DRAWN OVER A HOSTED PLUGIN EDITOR
//  (Sean, 21:20 on 28 Oct 2026 - the Link's picker opened BEHIND a Tube-Tech CL 1B)
// =============================================================================================
//  His words:
//
//      *"on the Link's own window (not the V2): with Tube-Tech CL 1B open in the Link's slot
//      view, pressing + opens the plugin picker BEHIND the CL 1B's native editor. Only the
//      bottom of the list shows below it ("1514 of 1514 plugins", EchoJay Auto Pan...
//      Saturation). The same fault was fixed in the V2."*
//
//  ---- WHY IT IS NOT A Z-ORDER BUG, WHICH IS THE WHOLE REASON THIS FILE EXISTS ---------------
//
//  A hosted plugin's editor is a HEAVYWEIGHT NSView living inside our window, and a heavyweight
//  NSView composites OVER every lightweight JUCE component no matter what the z-order says. So
//  `toFront()` on the picker cannot win, and neither can any amount of addAndMakeVisible
//  ordering: there is nothing to reorder, because the two are not in the same drawing system.
//
//  The V2 learnt this twice and the comments there record both dates:
//
//    2 Sep 2026   its rack menu started CLOSING inline hosted editors, for this reason.
//    25 Sep 2026  (21t-e) its PICKER did the same, after Sean's UnFairchild drew over it. Until
//                 then the picker only handled the POP-OUT case and so still drew behind an
//                 inline editor.
//
//  The Link's editor never got either half, which is what he is looking at tonight.
//
//  ---- THE TWO CASES ARE DIFFERENT, AND ONLY ONE OF THEM CAN BE SOLVED POLITELY --------------
//
//    A POP-OUT editor is a top-level always-on-top window. Nothing needs closing: drop
//    always-on-top and send it back, and raise it again when the popup has gone. The editor
//    keeps its state and his place in it, which is why this case is handled gently.
//
//    An INLINE editor is an NSView inside our own window. There is no "send it back" - see
//    above - so it is CLOSED. That is a real cost (his open plugin disappears) and it is still
//    the right trade: a picker he cannot read is worse than an editor he can reopen, and the V2
//    has behaved this way since September.
//
//  ---- ONE RULE, ONE PLACE ---------------------------------------------------------------------
//
//  The decision is this file's; the two actions are this file's; both editors call it. The V2's
//  picker had the rule written inline, which is exactly how the Link came to be missing it - a
//  rule that lives at one call site is a rule the next call site does not have.
// =============================================================================================

#include <JuceHeader.h>

namespace echojay
{

/** What one popup-over-a-guest needs doing. Both false = nothing is in the way. */
struct GuestPopupPrep
{
    bool lowerPopout = false;   ///< a top-level always-on-top editor window is up: lower it
    bool closeInline = false;   ///< a heavyweight NSView editor is inside our window: close it
    const char* why = "nothing is hosting an editor over this popup";
};

/** The pure decision. Nothing here reads the world, so the guard can hold every combination.

    BOTH can be true at once: a pop-out for one slot and an inline editor for another is an
    ordinary state in the V2's rack, and a popup drawn then has to deal with both or it comes up
    behind the one that was missed. */
inline GuestPopupPrep guestPopupPrep (bool hasPopout, bool hasInlineEditor)
{
    GuestPopupPrep p;
    p.lowerPopout = hasPopout;
    p.closeInline = hasInlineEditor;
    if (hasPopout && hasInlineEditor)
        p.why = "a pop-out editor window AND an inline hosted editor are both up";
    else if (hasPopout)
        p.why = "a pop-out editor window is always-on-top over this popup";
    else if (hasInlineEditor)
        p.why = "an inline hosted editor is a heavyweight NSView and composites over this popup";
    return p;
}

/** ---- WHAT A POPUP TOOK DOWN, SO IT CAN BE PUT BACK (Sean's R7 follow-up, 29 Oct 2026) -----

    *"the hosted plugin window is hidden to let the menu show, but it doesn't come back when the
    menu closes. Restore the editor as soon as any menu or popover closes, whether by a pick, Esc,
    a click outside or the menu dismissing itself, in both the V2 and the Link."*

    The pop-out half always came back - `onGone` fires from the picker's destructor, every death
    path. The INLINE half never did: it is CLOSED (see above: there is no sending a heavyweight
    NSView behind a lightweight one) and nothing remembered that it had been open, so his plugin
    simply vanished and he had to double-click the card again.

    So the prepare now RETURNS what it took down, as one value the caller holds for the life of the
    popup, and the restore puts both halves back. A hold with nothing in it restores nothing. */
struct GuestPopupHold
{
    bool loweredPopout   = false;   ///< a pop-out window was lowered: raise it again
    int  closedInlineSlot = -1;     ///< the slot whose inline editor was closed, or -1
};

/** Do it, over either editor's panel.

    A TEMPLATE because the two panels are different types - ChainListPanel in the V2,
    LinkChainPanel in the Link - that publish the same four calls (`hasInlineEditor()`,
    `inlineEditorSlot()`, `closeAllEditors()` and `showInline()`). A template keeps one body for
    both without either plugin's panel having to learn about the other's.

    `popout` is the panel's pop-out editor window or nullptr. The returned hold is what the caller
    passes back to restoreAfterPopup when the popup dies - by ANY route, which is why every call
    site hangs it on the one callback that fires on all of them rather than on the pick.

    NOTE the asymmetry, which is deliberate and explained at the top of this file: the pop-out is
    lowered (recoverable in place) while the inline editor is CLOSED and REOPENED, because there is
    no way to put a heavyweight view behind a lightweight one for the few seconds a menu is up. */
template <typename PanelT>
inline GuestPopupHold prepareForPopupOverGuest (PanelT& panel, juce::Component* popout, const char* tag)
{
    const auto prep = guestPopupPrep (popout != nullptr, panel.hasInlineEditor());
    GuestPopupHold hold;
    if (prep.lowerPopout && popout != nullptr)
    {
        popout->setAlwaysOnTop (false);
        popout->toBack();
        hold.loweredPopout = true;
    }
    if (prep.closeInline)
    {
        // READ IT BEFORE CLOSING: closeAllEditors clears the slot it is being asked for.
        hold.closedInlineSlot = panel.inlineEditorSlot();
        panel.closeAllEditors();
    }
    if (prep.lowerPopout || prep.closeInline)
        EchoJay_NSLog ((juce::String ("EJGuestPopup: ") + tag + " - " + prep.why
                        + (hold.loweredPopout ? "; pop-out lowered" : "")
                        + (prep.closeInline ? "; inline editor on slot "
                                              + juce::String (hold.closedInlineSlot + 1)
                                              + " closed, and it is REOPENED when the popup goes"
                                            : juce::String())).toRawUTF8());
    return hold;
}

/** Put back whatever that popup took down. Safe to call more than once: the hold is cleared as it
    is spent, so a path that both picks and then dismisses does not reopen twice.

    CALLED ON EVERY DEATH PATH, which is the whole of his R7 follow-up: a PopupMenu's async
    callback fires with 0 when it is dismissed, a picker's onGone fires from its destructor, and a
    CallOutBox's content component is deleted when the box goes - so each call site has one place
    that cannot be skipped by Esc or by a click outside. */
template <typename PanelT>
inline void restoreAfterPopup (PanelT& panel, juce::Component* popout, GuestPopupHold& hold)
{
    if (hold.loweredPopout && popout != nullptr)
    {
        popout->setAlwaysOnTop (true);
        popout->toFront (false);
    }
    hold.loweredPopout = false;
    if (hold.closedInlineSlot >= 0)
    {
        const int slot = hold.closedInlineSlot;
        hold.closedInlineSlot = -1;          // spent BEFORE the call, so a reentrant path cannot loop
        panel.showInline (slot);
        EchoJay_NSLog (("EJGuestPopup: the popup has gone, so the inline editor on slot "
                        + juce::String (slot + 1) + " is reopened (his R7 follow-up: it was hidden "
                        "to let the menu show and never came back)").toRawUTF8());
    }
}

/** The old shape, kept for the pop-out-only callers that have no hold to carry. */
template <typename PanelT>
inline void restorePopoutAfterPopup (PanelT& panel, juce::Component* popout, bool wasLowered)
{
    juce::ignoreUnused (panel);
    if (! wasLowered || popout == nullptr) return;
    popout->setAlwaysOnTop (true);
    popout->toFront (false);
}

/** ---- WHAT A + PRESS SAYS WHILE THAT LINK IS ALREADY BUSY (29 Oct 2026) --------------------

    This sentence took three passes in one night and every fault in it was mine:

      1. it did not exist - the press returned in silence, which cost an hour of my own and nearly
         put a wrong sentence about the Link in front of Sean;
      2. then it said "it will take this one when that lands", which is a LIE - nothing queues the
         press, it is dropped;
      3. then it said "still adding  to this Link" for a remove or a bypass, because addName is
         filled only for an add while the guard matches ANY pending change - a blank name and the
         wrong verb, in the cases it was likeliest to meet.

    So it is pure and legged here rather than composed inline a fourth time. It promises nothing,
    names what is actually happening, and gets the preposition right for each kind. */
inline juce::String inFlightRefusalLine (bool isAdd, bool isRemove, const juce::String& addName)
{
    const auto what = isAdd
                        ? ((addName.isNotEmpty() ? "still adding " + addName
                                                 : juce::String ("still adding a plugin"))
                           + " to this Link")
                        : isRemove ? juce::String ("still removing a plugin from this Link")
                                   : juce::String ("still changing this Link");
    return "Not added: " + what + " - try again in a moment.";
}

} // namespace echojay
