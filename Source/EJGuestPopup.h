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

/** Do it, over either editor's panel.

    A TEMPLATE because the two panels are different types - ChainListPanel in the V2,
    LinkChainPanel in the Link - that publish the same two calls (`hasInlineEditor()` and
    `closeAllEditors()`). A template keeps one body for both without either plugin's panel having
    to learn about the other's.

    `popout` is the panel's pop-out editor window or nullptr. Returns TRUE when a pop-out was
    lowered, which is the one thing the caller has to undo when its popup goes away: an editor
    window left behind the main window is its own fault report.

    NOTE the asymmetry, which is deliberate and explained at the top of this file: the pop-out is
    lowered (recoverable, and the caller restores it) while the inline editor is CLOSED (there is
    no way to put a heavyweight view behind a lightweight one). */
template <typename PanelT>
inline bool prepareForPopupOverGuest (PanelT& panel, juce::Component* popout, const char* tag)
{
    const auto prep = guestPopupPrep (popout != nullptr, panel.hasInlineEditor());
    bool lowered = false;
    if (prep.lowerPopout && popout != nullptr)
    {
        popout->setAlwaysOnTop (false);
        popout->toBack();
        lowered = true;
    }
    if (prep.closeInline)
        panel.closeAllEditors();
    if (prep.lowerPopout || prep.closeInline)
        EchoJay_NSLog ((juce::String ("EJGuestPopup: ") + tag + " - " + prep.why
                        + (lowered ? "; pop-out lowered" : "")
                        + (prep.closeInline ? "; inline editor closed" : "")).toRawUTF8());
    return lowered;
}

/** Put a lowered pop-out back, if one was lowered. The counterpart of the return above, so a
    caller never has to remember which two calls make a restore. */
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
