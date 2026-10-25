#pragma once
// =============================================================================================
//  EJShieldWork.h - WHAT THE READ-ONLY SHIELD ACTUALLY HAS TO DO THIS TICK
//  (his 17:25 fault, 25 Oct 2026)
// =============================================================================================
//  Sean:
//
//      *"As soon as the shield "an agent is working on this rack - stop it, or wait" came up, the
//      slot meters glitched and the plugin's window in the V2 glitched."*
//
//  ---- WHY A STATE CHANGE SHOWED AS A GLITCH ---------------------------------------------------
//
//  The shield is set from ONE place, `attachNative`, and the panel's own timer calls that every
//  100 ms. That part is by design: attach is the one place that reparents the plugin's view, and a
//  shield that is not the last subview blocks nothing, so the shield is re-asserted wherever the
//  attachment is. What was NOT by design is that the re-assertion was unconditional:
//
//      [shield setCaption:...];
//      [shield removeFromSuperview];                                  // <- every tick
//      [container addSubview:shield positioned:NSWindowAbove ...];     // <- every tick
//      [shield setNeedsDisplay:YES];                                   // <- every tick
//
//  So for as long as the shield was up, a sibling view over a hosted AU's own NSView was removed
//  from the view tree and added back TEN TIMES A SECOND, each time with a forced redraw. That is a
//  recomposite of that whole region at 10 Hz - which is what he saw on the plugin's view, and on
//  the slot meters drawn in the same card. Nothing was wrong with the meters or with the plugin;
//  they were being composited under a view that kept leaving and rejoining the hierarchy.
//
//  ---- THE RULE ------------------------------------------------------------------------------
//
//  Re-assert only what is actually wrong. The shield has to exist, sit on the plugin's rect, carry
//  the current words and be topmost - and on a steady tick all four are already true, so the
//  honest amount of work is NONE. The ordering step is kept for exactly the case its comment
//  names (a re-attach added the plugin's view after ours), and that case is now DETECTED rather
//  than assumed on every tick.
//
//  The decision is here, pure, so a guard holds both directions: a steady tick does nothing, and a
//  tick after a re-attach still puts the shield back on top. The AppKit calls are NativeClip.mm's.
// =============================================================================================

namespace echojay
{

/** What one setShield call must do. Every field false = the shield is already correct and the
    tick is a no-op, which is the normal case and was the glitch. */
struct ShieldWork
{
    bool create    = false;   // no shield exists yet
    bool setFrame  = false;   // it exists but is not on the plugin's rect
    bool setCaption= false;   // the words have changed
    bool reorder   = false;   // something was added above it (a re-attach)
    bool redraw    = false;   // true only when one of the above changed something visible
    bool remove    = false;   // the shield must come down

    bool nothingToDo() const
    { return ! create && ! setFrame && ! setCaption && ! reorder && ! redraw && ! remove; }
};

/** `on`            - the policy says the view is read-only right now.
    `exists`        - a shield view is already in the container.
    `frameMatches`  - its frame is the plugin's rect.
    `captionMatches`- its words are the ones the policy just gave.
    `isTopmost`     - it is the LAST subview, so nothing is drawn over it. */
inline ShieldWork shieldWorkFor (bool on, bool exists, bool frameMatches,
                                 bool captionMatches, bool isTopmost)
{
    ShieldWork w;
    if (! on) { w.remove = exists; return w; }
    if (! exists)
    {
        w.create = true; w.setCaption = true; w.redraw = true;
        return w;                      // a new view is added on top; no reorder needed
    }
    w.setFrame   = ! frameMatches;
    w.setCaption = ! captionMatches;
    w.reorder    = ! isTopmost;
    // A FRAME CHANGE REDRAWS ITSELF (AppKit invalidates on setFrame) and so does joining the view
    // tree; only new WORDS on an unchanged view need the redraw asked for.
    w.redraw     = w.setCaption;
    return w;
}

// =============================================================================================
//  ...AND THE SAME RULE FOR THE CLIP CONTAINER ITSELF (his overnight item 1b, 26 Oct 2026)
// =============================================================================================
//  Sean: *"the wobble: something still re-sets the hosted view's frame or order on the 100 ms
//  timer; a steady tick must change nothing."*
//
//  He is right, and the shield was only half of it. NativeClip2_attach is called from the panel's
//  timer every 100 ms, and while the two FRAME writes in it were already guarded by NSEqualRects,
//  six other things were not:
//
//      [container setBoundsOrigin:...];            // every tick
//      [container setWantsLayer:YES];              // every tick
//      [container layer].masksToBounds = YES;      // every tick
//      [container layer].backgroundColor = ...;    // every tick
//      [container layer].borderColor     = ...;    // every tick
//      [container layer].borderWidth     = 1.0;    // every tick
//      [container layer].cornerRadius    = 8.0;    // every tick
//
//  Every assignment to a CALayer property marks that layer as needing display and commits it on
//  the next frame. Seven of them, ten times a second, on the layer that CONTAINS the hosted AU's
//  own view: the region is recomposited continuously whether anything changed or not. That is the
//  wobble, and it is why it is there whether or not a run is going - the shield fix cured the
//  flicker he saw appear WITH the shield and could not have touched this.
//
//  The comment on the frame write says the intent exactly - *"reassert every call in case anything
//  (autoresizing, the plugin, JUCE) touched it"* - and that intent is kept. Re-asserting means
//  putting it back when it is WRONG, which is a comparison; writing it unconditionally is not
//  re-assertion but continuous mutation.
struct ClipWork
{
    bool setContainerFrame = false;
    bool setBoundsOrigin   = false;
    bool setWantsLayer     = false;
    bool setMasksToBounds  = false;
    bool setStyle          = false;   // background, border colour/width, corner radius
    bool setPluginFrame    = false;

    bool nothingToDo() const
    { return ! setContainerFrame && ! setBoundsOrigin && ! setWantsLayer
          && ! setMasksToBounds && ! setStyle && ! setPluginFrame; }
};

/** Every input is "is this already what we want?", so a steady tick answers nothing-to-do.

    `styleMatches` is one flag for the four layer-style properties deliberately: they are written
    as a set from the same constants, so they are either all ours or the layer is new. */
inline ClipWork clipWorkFor (bool containerFrameMatches, bool boundsOriginMatches,
                             bool hasLayer, bool masksToBounds, bool styleMatches,
                             bool havePlugin, bool pluginFrameMatches)
{
    ClipWork w;
    w.setContainerFrame = ! containerFrameMatches;
    w.setBoundsOrigin   = ! boundsOriginMatches;
    w.setWantsLayer     = ! hasLayer;
    w.setMasksToBounds  = ! masksToBounds;
    w.setStyle          = ! styleMatches;
    w.setPluginFrame    = havePlugin && ! pluginFrameMatches;
    return w;
}

} // namespace echojay
