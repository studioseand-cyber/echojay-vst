#pragma once
// =============================================================================================
//  EJLiveControl.h - CONTROL through the live view (stages C1-C3, 24 Oct 2026)
// =============================================================================================
//  Sean, after FOLLOW layer 2 passed at 21:31:
//
//      *"Now build CONTROL: turning a knob in the V2's live view changes the real plugin on the
//      Link at once (I hear it, the Link keeps it), and the "View only" lock comes off for
//      parameters. Keep your design's rules: deeper non-parameter changes say "open it on the
//      Link" with the button; control refused with the reason while an agent run holds the rack;
//      gestures framed so a drag is one change."*
//
//  ---- THE WIRE WAS FINISHED TWO WEEKS AGO; THE LAST TEN LINES WERE NOT -----------------------
//
//  `echojay::cmdring` has carried `Kind::kSetParam` since 10 Oct, with the gesture framing
//  (kBegin/kStream/kEnd, one id per drag) and the stale-structure discard. The Link DRAINED it and
//  threw it away, by ruling, with a count so it could not vanish silently:
//
//      *"NOT APPLIED HERE, BY RULING. A device parameter is a plugin call and can never be made
//      real-time safe, so it is staged for the message thread and the contract says so."*
//
//  Staged for the message thread - and nothing on the message thread ever read it. That ruling is
//  kept exactly as written: the audio thread now copies the frame into a lock-free staging ring and
//  the MESSAGE thread applies it. No plugin call moves onto the audio thread.
//
//  ---- WHY CONTROL IS ONLY OFFERED WHILE THE VIEW IS FOLLOWING --------------------------------
//
//  A value travels by the parameter's own INDEX. That is only meaningful if both processes hold
//  the same plugin with the same parameter list, and the thing that already proves it is FOLLOW
//  layer 2's schema digest (EJShadowValues.h). So the rule is one sentence: if the knobs are not
//  following, they do not control either. One fact, one owner - and the refusal already has words.
//
//  It also gives the acknowledgement for free. The design note's first rule was *"the copy is
//  never the authority"*: a gesture moves the copy because the plugin moves itself, but the value
//  that counts is the Link's, and when the Link's lands the mirror writes it back. That is layer 2
//  doing its ordinary job, not a second mechanism.
//
//  ---- WHAT CONTROL DOES NOT COVER, SAID UP FRONT ---------------------------------------------
//
//  A change the plugin makes to its own opaque state without reporting a parameter - a preset
//  recall, a graph drag, an IR load - CANNOT travel this way, and the design note's §4 ruling is
//  that the honest answer is to say so rather than ship a road that loses it. This build does not
//  yet DETECT such a change (that is layer 3), so the line is a standing one on the card and not a
//  claim to have noticed anything: it says what control covers and where to go for the rest.
#include <JuceHeader.h>
#include "EJCmdRing.h"
#include <vector>

namespace echojay
{

// ---- THE DECISION: MAY A GESTURE ON THE LIVE VIEW TRAVEL RIGHT NOW? -------------------------

struct LiveControlPolicy
{
    bool         allow = false;
    juce::String line;          // empty only when `allow`; the words he reads when it is refused
};

/** Pure, so the gate holds every road without a Link, a plugin, a run or a window.

    `following`    - FOLLOW layer 2 is applying this slot's values, which is what proves the two
                     parameter lists agree (and is false for an older Link or a digest mismatch).
    `runHoldsRack` - an agent run holds this rack right now.

    THE ORDER MATTERS AND IS DELIBERATE: the run is named FIRST. If a run holds the rack, that is
    what he needs to know, even if the mirror also happens to be down - telling him about a
    parameter list while an agent is dialling the thing would be the less useful of two true
    answers. */
/** Does a run still block the user's own knobs? (his 17:25 fault, 25 Oct 2026)

    *"After the run said "Stopped.", the shield never cleared: the rack still says an agent is
     working on it and the plugin can't be used."*  And his ruling: *"a stopped run must always
     release the rack."*

    THE SHIELD WAS READING THE WRONG FACT, and the fact itself was right. `runNeedsItsRack()` is
    `isActive() || canUndoAll()`, and the second half is deliberate: a FINISHED run still needs the
    rack HELD while its "Undo all" is live, because that button walks the rack back to the run's
    checkpoint, and "Undo all showed but could not be pressed" was his fault of 23 Oct. But the
    live-view shield read that same pair, so after a stop it went on refusing his knobs with the
    words "an agent is working on this rack - stop it, or wait" - a sentence that was untrue in
    every part: nothing was working, he had already stopped it, and there was nothing to wait for.

    TWO QUESTIONS, TWO ANSWERS. "Who must hold this rack" keeps the undo-all half; "whose gesture
    may move this plugin" is only ever about a run that is RUNNING. A rack held so that Undo all
    stays pressable is not a rack in use, and it takes no gestures away from him. */
inline bool runBlocksUsersKnobs (bool runIsActive, bool undoAllStillLive)
{
    juce::ignoreUnused (undoAllStillLive);
    return runIsActive;
}

inline LiveControlPolicy liveControlPolicy (bool following, bool runHoldsRack)
{
    if (runHoldsRack)
        return { false, "an agent is working on this rack - stop it, or wait" };
    if (! following)
        return { false, "this view is not following the plugin on the Link, so a knob here would "
                        "not reach it - open it on the Link instead" };
    return { true, {} };
}

// ---- FOLLOW NEVER FIGHTS A GESTURE (his overnight item 1a, 26 Oct 2026) --------------------
//
//  Sean: *"the live view is still glitchy: the plugin's window wobbles, and turning a knob by hand
//  isn't smooth, it jumps."*  And his own reading of it, which is right:
//
//      *"follow fighting control: my gesture goes to the Link, the Link republishes at 30 Hz, and
//       the 20 Hz follow tick writes the Link's slightly older value back into the copy mid-drag.
//       Rule: while a gesture is in progress on a parameter (begin to end, plus a short settle),
//       follow never writes that parameter into the copy, and echoes of my own writes are ignored."*
//
//  It was true by construction: shadowApplyChangedValues walked every parameter the frame carried
//  and had no notion of a gesture at all. So a drag went copy -> ring -> Link -> publish -> copy,
//  and the value that came back was whatever the Link held one or two frames ago. At 20 Hz against
//  a hand moving continuously that is a write BACKWARDS into the knob he is holding, ten times a
//  second: the jump he feels, and on some plugins a redraw storm with it.
//
//  THE SETTLE IS PART OF THE RULE, not a safety margin. When the gesture ends, frames composed
//  before the last value left are still in flight; applying one of those is the same backward
//  write with better timing. The hold therefore outlives the gesture by kFollowSettleMs, after
//  which the Link is the authority again - which is the design's first rule ("the copy is never the
//  authority") kept intact, because the hold is per PARAMETER and never stops the other knobs
//  following.
static constexpr double kFollowSettleMs = 250.0;

/** May FOLLOW write this parameter into the copy right now?

    `gestureInProgress`      - a gesture on this parameter has begun and not ended.
    `msSinceGestureEnded`    - how long ago the last one ended; <0 when none ever has.
    `settleMs`               - kFollowSettleMs, passed so a guard can state it. */
inline bool followMayWriteParam (bool gestureInProgress, double msSinceGestureEnded,
                                 double settleMs = kFollowSettleMs)
{
    if (gestureInProgress) return false;
    if (msSinceGestureEnded >= 0.0 && msSinceGestureEnded < settleMs) return false;
    return true;
}

/** The standing line for a live view that CAN control. Not a detection of anything: it states what
    travels and where the rest is done. Layer 3 is what turns the second half into a notice. */
inline const char* liveControlCoversLine() noexcept
{
    return "Knobs here go to the plugin on the Link. For presets, graphs and anything deeper, "
           "open it on the Link.";
}

// ---- THE COALESCER, ON THE MESSAGE THREAD ---------------------------------------------------

/** One parameter's staged move. `sawBegin`/`sawEnd` are the gesture boundaries, which the
    coalescer may never discard - they are what makes one drag one undo step. */
struct StagedParam
{
    int          slot    = -1;
    juce::uint32 paramId = 0;
    double       value   = 0.0;
    bool         sawBegin = false;
    bool         sawEnd   = false;
    juce::uint32 gesture = 0;
};

/** BY PARAMETER, NEVER BY FRAME. A 400-frame drag applies as the latest value per (slot, paramId)
    per tick; the boundaries are OR-ed in rather than dropped, so a gesture that began and ended
    inside one tick is still one gesture and not a value with no story.

    Order is preserved by first appearance, so a log reads in the order he moved things. */
inline std::vector<StagedParam> coalesceStagedParams (const StagedParam* in, int n)
{
    std::vector<StagedParam> out;
    if (in == nullptr || n <= 0) return out;
    out.reserve ((size_t) juce::jmin (n, 32));

    for (int i = 0; i < n; ++i)
    {
        const auto& f = in[i];
        bool merged = false;
        for (auto& o : out)
            if (o.slot == f.slot && o.paramId == f.paramId)
            {
                o.value    = f.value;                       // the latest wins
                o.sawBegin = o.sawBegin || f.sawBegin;      // a boundary is never dropped
                o.sawEnd   = o.sawEnd   || f.sawEnd;
                if (f.gesture != 0) o.gesture = f.gesture;
                merged = true;
                break;
            }
        if (! merged) out.push_back (f);
    }
    return out;
}

/** A frame's phase as the two booleans the coalescer carries. */
inline StagedParam stagedFromFrame (int slot, juce::uint32 paramId, double value,
                                    juce::uint32 phase, juce::uint32 gesture)
{
    StagedParam s;
    s.slot     = slot;
    s.paramId  = paramId;
    s.value    = value;
    s.sawBegin = (phase == echojay::cmdring::Phase::kBegin);
    s.sawEnd   = (phase == echojay::cmdring::Phase::kEnd);
    s.gesture  = gesture;
    return s;
}

// ---- A GESTURE THAT THE PLUGIN NEVER FRAMES -------------------------------------------------

/** Many plugins report a parameter change and never report a gesture at all, so a drag arrives as
    a burst of changes with no beginning and no end. His requirement is that a drag is ONE change,
    so one is inferred: the first change opens a gesture, and a gesture with no movement for
    `kLiveControlQuietMs` is closed and recorded as one step.

    150 ms is chosen to be longer than the gap between two frames of a mouse drag (a 60 Hz drag is
    16 ms apart, and even a slow hand is well inside this) and shorter than the gap between two
    deliberate moves of the same knob. It is a judgement, it is stated here rather than buried, and
    a plugin that DOES frame its gestures never reaches it. */
static constexpr int kLiveControlQuietMs = 150;

} // namespace echojay
