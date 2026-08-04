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

} // namespace ejmap
