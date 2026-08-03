/*
  ==============================================================================

    EjmapProbeRoute.h — the verdict fork, and floors that carry their unit.

    Pure decision logic, lifted out of EjmapProbe.h for the same reason
    EjmapTriage.h was: the drift gate must be able to compile it without
    linking the message loop, so that what it decides is provable in a test
    rather than only on hardware. Handing eq to this fork made that necessary --
    the hazard it introduced is a floor in the wrong unit, and the proof is two
    calls with identical measurements and different floors.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

namespace ejmap
{
namespace route
{

enum class Route { deafness, overClaim, tracks };

/** A FLOOR CARRYING ITS UNIT.

    THE STANDING-QUESTION ANSWER for converting eq to emitVerdict, measured
    before the conversion landed. routeVerdict is DIMENSIONLESS: it decides
    deafness by `|moved| < 4*floor` and over-claim by
    `||moved| - |pred|| > tolerance`, and nothing in either comparison knows
    whether the numbers are decibels or octaves.

    eq's assertions used only the TOLERANCE -- `|meas - pred| <= tol`, a
    binary pass. Handing eq to the fork introduces a SECOND input those
    assertions never consulted: the floor. And the floor is what decides
    between `contradicts` and `inconclusive`.

    So the newly falsifiable thing is a floor in the wrong unit. eq's centre
    floor is 0.0322 oct (4x = 0.1288); the depth floor is 0.088 dB (4x =
    0.352). A centre feature that moved 0.20 oct against a predicted 2.0:
      - with the octave floor  -> 0.20 > 0.1288 -> overClaim -> CONTRADICTS
      - with the dB floor      -> 0.20 < 0.352  -> deafness  -> INCONCLUSIVE
    Same measurements, opposite verdict, decided entirely by which floor was
    passed. 5a made the UNIT required at emit, but the floor was a bare
    double with no unit, so that guard did not cover this.

    Pairing the value with its unit makes the mismatch a loud failure at the
    choke point instead of a silent flip. */
struct Floor
{
    double value = 0;
    juce::String unit;
    Floor() = default;
    Floor (double v, const juce::String& u) : value (v), unit (u) {}
};

inline Route routeVerdict (double movedDb, double floorDb, double predDb,
                           double tolerance)
{
    if (std::abs (movedDb) < 4.0 * juce::jmax (floorDb, 1.0e-9))
        return Route::deafness;                    // did not move -> carve-out 1
    if (std::abs (std::abs (movedDb) - std::abs (predDb)) > tolerance)
        return Route::overClaim;                   // moved, wrong magnitude
    return Route::tracks;
}

/** The words each route is allowed to use, in one place so no suite can
    invent its own. Carve-out 1's exclusions are named in the deafness
    text because they are what must run before any promotion.
*/
/** `unit` is REQUIRED and comes from the caller. This function used to
    write "dB" into every route sentence, which was false for comp's ratio
    verdict -- a slope delta, dimensionless -- printing "moved 0.38 dB
    against 0.40 dB predicted" about a ratio. It would be false again for
    eq (octaves) and for attack/release (ms). Fixed explanatory text at a
    shared exit is the same trap as a fixed inconclusive basis: it answers
    confidently for callers it knows nothing about, and only the caller
    knows the units of its own measurement. */
inline juce::String routeText (Route r, double movedDb, double predDb, double floorDb,
                               const juce::String& unit)
{
    const juce::String u = unit.isEmpty() ? juce::String() : " " + unit;
    switch (r)
    {
        case Route::deafness:
            return "INCONCLUSIVE: possibly mode-suppressed -- the feature did not move above "
                   "its floor (" + juce::String (std::abs (movedDb), 2) + u + " against 4*sigma "
                 + juce::String (4.0 * floorDb, 2) + u + ") while " + juce::String (predDb, 2)
                 + u + " was predicted. Carve-out 1 governs: promotion to contradicts requires "
                   "(a) no suppressing mode in the map AND (b) no gesture evidence at this index";
        case Route::overClaim:
            return "OVER-CLAIM: the feature MOVED (" + juce::String (std::abs (movedDb), 2)
                 + u + ", above its floor) but by the wrong magnitude against "
                 + juce::String (std::abs (predDb), 2) + u + " predicted";
        case Route::tracks:
        default:
            return "tracks: moved " + juce::String (std::abs (movedDb), 2)
                 + u + " against " + juce::String (std::abs (predDb), 2) + u + " predicted";
    }
}


} // namespace route
} // namespace ejmap
