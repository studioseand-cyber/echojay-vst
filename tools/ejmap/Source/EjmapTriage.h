/*
  ==============================================================================

    EjmapTriage.h — naming causes apart, and checking a map's claims about a
    control against what the signal did.

    PURE DECISION LOGIC. Nothing here renders, writes or touches an audio type:
    it takes numbers and lambdas and returns a classification with the cause in
    words. That is deliberate -- it lives outside EjmapProbe.h so the drift gate
    can compile it without linking the message loop, and every state it can
    report is provable in a test rather than only on hardware. The
    write-did-not-land state in particular cannot be forced on a real plugin,
    because writes land.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

namespace ejmap
{
namespace triage
{

//==========================================================================
/** CAUSE TRIAGE: one symptom, three causes, named apart.

    "The control did nothing" is produced by at least three unrelated
    failures, and M9 has reported all three with the same words:

      1. the index is outside this instance      -> refused at lookup
      2. the write never landed                  -> the writer reports it
      3. the write landed and the control is inert

    Only (3) is a statement about the plugin. (1) is a statement about the
    map and (2) about the bridge, and both have been read as plugin
    deafness on this project. A verdict built on the wrong one of these
    attributes a map defect to a plugin, or a bridge defect to a map.

    This is DIAGNOSTIC rather than functional, which is exactly why it is
    built first: it is the part that gets dropped when a session runs long,
    and everything downstream quotes its answer.

    `write` returns the landing time in ms, negative when the write never
    landed -- the contract writeAndServiceRunloop already has. `measure`
    returns one scalar feature; the caller chooses which, because only the
    caller knows what this control is supposed to move.
*/
enum class Liveness { indexOutOfRange, writeDidNotLand, landedButInert, live };

struct LivenessResult
{
    Liveness state = Liveness::landedButInert;
    double   movedBy = 0.0, floor = 0.0;
    juce::String detail;

    bool isLive() const { return state == Liveness::live; }

    /** The CAUSE in words, never the symptom. */
    juce::String cause() const
    {
        switch (state)
        {
            case Liveness::indexOutOfRange:
                return "the index is outside this instance's parameters -- a statement about "
                       "the MAP, not about the plugin";
            case Liveness::writeDidNotLand:
                return "the write never landed -- a statement about the WRITE PATH (bridge, "
                       "message thread), not about the plugin. Nothing was measured because "
                       "nothing was set";
            case Liveness::landedButInert:
                return "the write landed and the feature moved " + juce::String (movedBy, 3)
                     + " against a floor of " + juce::String (floor, 3)
                     + " -- the parameter IS at the value asked for and does nothing there. "
                       "This one is a statement about the PLUGIN or about what the map claims "
                       "this control is";
            case Liveness::live:
            default:
                return "the write landed and the feature moved " + juce::String (movedBy, 3)
                     + " against a floor of " + juce::String (floor, 3) + " -- live";
        }
    }
};

template <typename WriteFn, typename MeasureFn>
inline LivenessResult classifyLiveness (int index, int paramCount,
                                        float fromNorm, float toNorm, double floor,
                                        WriteFn&& write, MeasureFn&& measure)
{
    LivenessResult r;
    r.floor = floor;
    if (! juce::isPositiveAndBelow (index, paramCount))
    {
        r.state = Liveness::indexOutOfRange;
        r.detail = "index " + juce::String (index) + " of " + juce::String (paramCount);
        return r;
    }
    if (write (fromNorm) < 0) { r.state = Liveness::writeDidNotLand;
                                r.detail = "reference write"; return r; }
    const double a = measure();
    if (write (toNorm) < 0)   { r.state = Liveness::writeDidNotLand;
                                r.detail = "probe write"; return r; }
    const double b = measure();
    r.movedBy = std::abs (b - a);
    r.state = (r.movedBy > floor) ? Liveness::live : Liveness::landedButInert;
    return r;
}

//==========================================================================
/** ROLE VERIFICATION BY SIGNAL, and the limit of it.

    A map's `role` is a CLAIM. Before a suite acts on it -- and eq's
    falsifier acts on it hard, writing the control and reading a verdict
    out of what moves -- the claim is checked against the signal.

    For `stereo_width`: the side band must move above its floor AND the mid
    band must not. That separates a width control from a gain (mid moves),
    from a filter (mid moves, localised) and from an inert index (nothing
    moves).

    WHAT IT CANNOT DO, stated wherever it is used rather than buried:
    it cannot separate the claimed width control from ANY OTHER width
    control. Two controls with the same measurable character are
    indistinguishable by that character, so a map naming the wrong one of
    two width controls passes this check and produces a correct verdict for
    the wrong reason. The role is SUPPORTED, never proven. Narrowing that
    further needs the index/name cross-check, which only checks the map
    against itself.
*/
struct RoleEvidence
{
    bool supported = false, midIsMinority = false, midAboveFloor = false;
    double sideMoved = 0, midMoved = 0, sideFloor = 0, midFloor = 0;
    juce::String role, controlName, why;

    juce::String limitStatement() const
    {
        return "LIMIT: this check separates a " + role + " control from a gain, a filter and "
               "an inert index. It CANNOT separate it from another " + role + " control, so "
               "the role is supported by measurement, not proven";
    }
};

inline RoleEvidence verifyStereoWidthRole (const juce::String& controlName,
                                           double sideMovedDb, double midMovedDb,
                                           double sideFloorDb, double midFloorDb)
{
    RoleEvidence e;
    e.role = "stereo_width";
    e.controlName = controlName;
    e.sideMoved = sideMovedDb; e.midMoved = midMovedDb;
    e.sideFloor = sideFloorDb; e.midFloor = midFloorDb;

    // THE MID CRITERION IS A RATIO, NOT AN ABSOLUTE. The first version of
    // this check required mid movement below 4*sigma_depth and FAILED THE
    // SIGNED AMEK FIXTURE: engaging Mono Maker moves the mid band 0.437 dB
    // against a 0.352 floor. That number is not a defect and is not news --
    // arm A already records it, with its cause, as "recorded, not a
    // criterion: M=(L+R)/2, so mono-ing below the crossover necessarily
    // moves side content into mid. Expected physics."
    //
    // A new check must not silently assign a threshold to a quantity an
    // existing check deliberately left unthresholded WITH a documented
    // physical cause. Doing so re-litigates a settled question, and the
    // first thing it disqualifies is the fixture the settlement came from.
    //
    // What actually separates a width control from a gain is DOMINANCE: a
    // gain moves mid and side together, a width control moves side while
    // mid stays a minority effect. 0.25 is the project's declared constant
    // for "a minority of the expressed change".
    const bool sideMoves = sideMovedDb > sideFloorDb;
    const bool midIsMinority = midMovedDb <= 0.25 * sideMovedDb;
    e.midIsMinority = midIsMinority;
    e.midAboveFloor = midMovedDb > midFloorDb;
    e.supported = sideMoves && midIsMinority;
    const bool midHolds = midIsMinority;

    if (! sideMoves && midMovedDb <= midFloorDb)
        e.why = "the map claims '" + controlName + "' is a stereo_width control; writing it "
                "moved the side band " + juce::String (sideMovedDb, 3) + " dB against a floor "
                "of " + juce::String (sideFloorDb, 3) + " and moved nothing else. The claim is "
                "NOT SUPPORTED: this control does nothing measurable here";
    else if (! sideMoves)
        e.why = "the map claims '" + controlName + "' is a stereo_width control; writing it "
                "moved the MID band " + juce::String (midMovedDb, 3) + " dB while the side band "
                "moved " + juce::String (sideMovedDb, 3) + " (floor " + juce::String (sideFloorDb, 3)
              + "). The claim is NOT SUPPORTED: this behaves like a level or filter control, "
                "not a width control";
    else if (! midHolds)
        e.why = "the map claims '" + controlName + "' is a stereo_width control; the side band "
                "moved " + juce::String (sideMovedDb, 3) + " dB as a width control should, but "
                "the MID band also moved " + juce::String (midMovedDb, 3) + " against a floor of "
              + juce::String (midFloorDb, 3) + ". The claim is NOT SUPPORTED as a CLEAN width "
                "control: the mid movement is not a minority of the side movement "
                "(limit 0.25x), so whatever else it does would contaminate a falsifier "
                "built on it";
    else
        e.why = "the map's stereo_width claim for '" + controlName + "' is supported: side "
                "moved " + juce::String (sideMovedDb, 3) + " dB (floor "
              + juce::String (sideFloorDb, 3) + "), mid moved "
              + juce::String (midMovedDb, 3) + " dB, which is "
              + juce::String (100.0 * midMovedDb / juce::jmax (1.0e-9, sideMovedDb), 1)
              + "% of the side movement (limit 25%)"
              + (midMovedDb > midFloorDb
                   ? ". RECORDED, NOT A CRITERION: the mid movement is above its own floor of "
                     + juce::String (midFloorDb, 3) + " dB. On a mono-maker this is expected "
                       "physics -- M=(L+R)/2, so collapsing content below the crossover moves "
                       "side energy into mid -- and arm A records the same quantity the same way"
                   : "");
    return e;
}

/** THE ENABLE NULL-TEST. An enable link is supposed to make one control
    live and change nothing else. A link pointing at a global bypass or a
    mode switch instead contaminates the arm that is not testing it -- the
    correct-map arm measures a different plugin and nobody is looking.

    An enable that changes the measurement is not an enable. */
struct EnableNull
{
    bool clean = false;
    double primaryMoved = 0, floor = 0;
    juce::String linkName, why;

    /** KNOWN LIMIT, printed wherever a link is used rather than only in the
        proposal. A wrong enable link is INVISIBLE when the control is already
        live in the instance's default state: the link makes no difference, so
        nothing can tell whether it was right. Measured on AMEK, whose Mono
        Maker is engaged by default -- a link pointed at an unrelated control
        passed. This test catches links that DO DAMAGE, not links that do
        nothing. */
    static juce::String limitStatement()
    {
        return "LIMIT: this test catches an enable link that does damage, not one that does "
               "nothing. A wrong link is invisible when the control is already live by default, "
               "because the link makes no difference and nothing can tell";
    }
};

inline EnableNull checkEnableIsNull (const juce::String& linkName,
                                     double primaryMovedDb, double floorDb)
{
    EnableNull n;
    n.linkName = linkName; n.primaryMoved = primaryMovedDb; n.floor = floorDb;
    n.clean = primaryMovedDb <= floorDb;
    n.why = n.clean
        ? "engaging '" + linkName + "' left the primary feature within its floor ("
          + juce::String (primaryMovedDb, 3) + " <= " + juce::String (floorDb, 3) + "). "
          + EnableNull::limitStatement()
        : "engaging '" + linkName + "' moved the PRIMARY feature by "
          + juce::String (primaryMovedDb, 3) + " dB against a floor of "
          + juce::String (floorDb, 3) + ". This is not a per-control enable: it changes what "
          "the whole measurement sees, so every arm that runs after it -- including the arms "
          "not testing this link -- measures a different plugin";
    return n;
}


} // namespace triage
} // namespace ejmap
