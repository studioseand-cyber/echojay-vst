/*
  EjmapViewLayer.h

  The native view layer beneath a hosted plugin editor, measured and then
  fixed. Two functions, both no-ops off macOS.

  WHY THIS EXISTS. Eiosis AirEQ left ejmap's toolbar, list and wizard BLACK
  while the plugin itself rendered correctly (screenshot, 4 Aug 2026). The
  editor was not covering them: a JUCE-layer snapshot taken with AirEQ attached
  showed toolbar and list drawn correctly in JUCE's own buffer while the screen
  showed nothing, and AirEQ's editor fitted the region it was given. JUCE was
  painting; the compositor was not showing it.

  That is the signature of a MIXED view hierarchy. When a hosted plugin's
  NSView is layer-backed and the JUCE peer above it is not, AppKit promotes the
  hierarchy and the peer's software drawing stops reaching the screen.

  describeViewTree() measures that state instead of arguing about it: it names
  every view under the peer with its class, frame, and whether it wants a
  layer. It is what says whether a given plugin trips the condition at all --
  and the difference between a plugin that breaks the window and one that does
  not should be visible in that dump.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace ejmap
{

/** One line per native view under this component's peer: class, frame,
    wantsLayer, whether a layer object exists. Empty string off macOS or with
    no peer. Reads only; changes nothing.
*/
juce::String describeViewTree (juce::Component& topLevel);

/** Gives each view hosted directly under the peer its OWN layer.

    THE FLAG GOES ON THE CONTAINER, NOT THE PEER -- measured, not assumed. The
    peer is already layer-backed (wantsLayer=YES, layer=yes) before any editor
    is attached, so setting it there does nothing. What differs between a
    plugin that breaks the window and one that does not is what arrives
    UNDERNEATH:

      API-550A (Waves, bridged)     AirEQ (breaks it)
        AUv2ContainerView   no        AuViewClass...    no
          NSRemoteView  layer=yes       JUCEView...  layer=nil

    The bridged plugin composites out-of-process into its own layer and is
    fine. AirEQ arrives as a NESTED JUCE peer view with no layer of its own,
    so it draws into the peer's backing store -- the same surface ejmap's
    toolbar and list are drawn into.

    Giving the hosted container its own layer separates the two surfaces. It
    is also the recipe this project already proved in the main plugin
    (NativeClip.mm: wantsLayer on the CONTAINER).

    Returns one line per view it touched, or a line saying it touched nothing.
    This must never be a silent mutation of how every editor composites.
*/
juce::String layerBackHostedViews (juce::Component& topLevel);

/** Stop AppKit's periodic-event generation, if anything started it.

    JUCE ends every quit in shutdownNSApp:

        [NSApp stop: nil];
        [NSEvent startPeriodicEventsAfterDelay: 0 withPeriod: 0.1];

    The periodic events are a trick to make nextEventMatchingMask return so the
    stop takes effect, and THEY ARE NEVER STOPPED. It is the only call to that
    API in the whole framework, and AppKit throws
    NSInternalInconsistencyException -- "Periodic events are already being
    generated" -- if a start arrives while some are running. An uncaught ObjC
    exception aborts the process, after all the work is done and the row is
    already written.

    Calling stop first makes JUCE's start legal whoever made the first start:
    a second quit, a plugin, or AppKit tracking. Stopping when none are running
    is a no-op.

    NOT A DIAGNOSIS OF THE FIRST CALLER. That is still unestablished -- ten
    single-plugin runs on a clone of the live root did not reproduce the abort.
    The mechanism is read from JUCE's source and named by the exception; who
    started the first set is not.
*/
void stopPeriodicEventsIfAny();

/** DIAGNOSTIC: capture the hosted editor's own NSView to a PNG, in-process.

    Not the window and not the screen: the VIEW. createComponentSnapshot draws
    the JUCE layer and leaves a hosted editor blank (M8, and confirmed here on
    AirEQ), while window-level capture is unavailable on this SDK --
    CGWindowListCreateImage is gone and screencapture cannot reach a window on
    another Space. An NSView can be asked to draw itself, which needs no
    permission and no window server.

    Returns a one-line report of what happened, including the fraction of
    non-background pixels, because a capture that "succeeds" and returns an
    empty rectangle is the failure mode that matters -- a bridged AU's content
    lives in another process and its NSRemoteView may draw nothing here.
*/
juce::String captureHostedEditor (juce::Component& topLevel, const juce::File& out);

/** What a capture actually produced, as numbers rather than a sentence.

    THE FIELD IS NAMED AFTER WHAT WAS OBSERVED, NEVER AFTER A CAUSE. An earlier
    reading of this measurement attributed an empty capture to out-of-process
    hosting, because API-550A was both empty and an NSRemoteView. It was
    disproved on 5 Aug 2026: the same vendor's VST3, loaded in-process with no
    NSRemoteView anywhere in the tree, captures at 0.0% too. A field called
    `unavailable_bridged` would have frozen that wrong cause into the corpus --
    the crash_on_load mistake again.

    So: `empty` says the rectangle came back blank. WHY is an open question, and
    the numbers here are what a later answer gets tested against.
*/
struct CaptureResult
{
    bool   attempted = false;
    bool   wrote     = false;
    int    width = 0, height = 0;
    double fraction = 0.0;      // non-background, 0..1
    juce::String note;          // the human line, for logs

    /** "ok" | "empty" | "unavailable". Never a cause. */
    juce::String state() const
    {
        if (! attempted)   return "unavailable";
        if (fraction <= 0.0) return "empty";
        return "ok";
    }
};

/** Same capture, reported as numbers. */
CaptureResult captureHostedEditorResult (juce::Component& topLevel, const juce::File& out);

} // namespace ejmap
