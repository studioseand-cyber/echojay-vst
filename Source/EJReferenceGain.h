#pragma once
// =============================================================================================
//  EJReferenceGain.h - THE AUDITIONED REFERENCE'S GAIN RULE, AND NOTHING ELSE
//  (Sean's 2c ruling, 30 Oct 2026; Kathy's 2c addendum; her R9-1 of 31 Oct)
// =============================================================================================
//  His ruling: REAL LEVEL by default. Flipping to the reference plays it at its true level and
//  nothing is matched unless he asks, through an optional "Level-match" toggle that is OFF by
//  default. Her addendum: the call must RETURN the gain actually applied, so her page can say
//  "Matched 24 of 30 dB" and have that be true.
//
//  WHY THIS FILE EXISTS AT ALL. The rule is read in three places - the message thread's setter, the
//  render thread's ramp target, and a leg that checks the arithmetic without a host - and it must be
//  the SAME rule in all three. It cannot live in EJMatchAudition.h, where the rest of the audition's
//  logic is, because that header reaches MatchHost and CaptureSnapshot and so cannot be included by
//  PluginProcessor.h (the cycle is real; it was the first thing I tried). So: one function, no
//  includes but JUCE, included by both sides.
// =============================================================================================

#include <JuceHeader.h>
#include <cmath>

namespace echojay
{

/** CUT ONLY, floored, and a non-finite request changes nothing.

    Cut only because boosting a reference to meet a quiet mix invents headroom that is not there and
    can clip the monitor path: a positive request is 0 dB, which is the reference's real level.
    Floored because a request past the floor should be the floor rather than silence - a reference
    he cannot hear is not a comparison. `currentDb` is returned when the request is refused, so a
    caller always learns the figure that is actually applied rather than the one it asked for. */
inline float clampReferenceGainDb (float wantedDb, float floorDb, float currentDb) noexcept
{
    if (! std::isfinite (wantedDb)) return currentDb;
    return juce::jlimit (floorDb, 0.0f, wantedDb);
}

} // namespace echojay
