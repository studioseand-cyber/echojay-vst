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

/** Makes the JUCE peer explicitly layer-backed, so a layer-backed plugin view
    cannot promote the hierarchy out from under it. Returns what it did, for
    the log: this must never be a silent mutation of how every editor
    composites.

    NOT a clip. Clipping constrains an oversized editor to its region and was
    the fix proposed while the symptom was believed to be overlap; the
    screenshot refuted overlap, and clipping cannot repaint a black toolbar.
*/
juce::String ensurePeerLayerBacked (juce::Component& topLevel);

} // namespace ejmap
