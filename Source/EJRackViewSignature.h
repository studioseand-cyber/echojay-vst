#pragma once
// =============================================================================================
//  EJRackViewSignature.h - HAS THE RACK IN VIEW CHANGED? (Sean's stale strip, 28/29 Oct 2026)
// =============================================================================================
//  His report:
//
//      *"After he removed an EchoJay Compressor from the Kick Link and the agent added Unfiltered
//      Audio Zip there, the V2's rack strip still showed "EchoJay C..." in slot 1 ... Reopening
//      the window showed the Zip correctly."*
//
//  ---- WHY THE STRIP DID NOT FOLLOW -----------------------------------------------------------
//
//  refreshLinkRackCache re-reads every Link's sidecar on the tick - the data was never stale. What
//  was stale was the PANEL, and it was rebuilt only when a TRIM had moved:
//
//      sig += outGainDb * 41 + preTrimDb * 43 + (i + 1)
//
//  There is no plugin NAME in that sum. A plugin REPLACED in the same slot - his exact case, one
//  device out and another in at slot 1, both sitting at 0 dB - produces the identical signature, so
//  nothing asked the panel to rebuild and it went on drawing the device that had gone.
//
//  The comment under that code says the rest out loud: an EQ arriving used to force a re-measure,
//  that was removed as a layout optimisation ("One less thing that can make geometry depend on
//  IO"), and the REBUILD went with it. A correct optimisation that quietly took a behaviour with it.
//
//  ---- SO THE QUESTION IS "IS THIS THE SAME RACK", NOT "HAVE THE TRIMS MOVED" ------------------
//
//  Identity first: what is in each slot, in order. Then the figures the strip draws. A signature
//  that misses a change makes a liar of the window; one that changes too often costs a rebuild a
//  second, which is cheap and harmless - so when in doubt this errs towards rebuilding.
// =============================================================================================

#include <JuceHeader.h>

namespace echojay
{

/** What the strip is drawing, as one string. Compare it across ticks; different means rebuild.

    INCLUDES, per slot and in order: the plugin's name (identity - the thing his case changed), its
    out gain and its pre-trim (the figures the rows show), and whether it is bypassed, because a
    bypassed row is drawn differently and a bypass toggled on the Link is exactly the kind of change
    the window must not sleep through.

    NOT a hash: a string of a handful of slots costs nothing at 1 Hz, and a collision here would
    cost a stale window again. kRegMaxSlots is 16; this is never long. */
inline juce::String rackViewSignature (const LinkShm::RackSidecar& r)
{
    if (! r.valid) return "<invalid>";
    juce::String sig;
    sig << r.slots.size() << ":";
    for (const auto& s : r.slots)
        sig << s.name << "|"
            << juce::String (s.outGainDb, 3) << "|"
            << juce::String (s.preTrimDb, 3) << "|"
            << (s.bypassed ? "b" : "-") << ";";
    return sig;
}

} // namespace echojay
