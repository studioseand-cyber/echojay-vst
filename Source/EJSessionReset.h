#pragma once
// =============================================================================================
//  EJSessionReset.h - A RACK SWITCH ALWAYS CONNECTS, OR SAYS WHY IN WORDS
//  (Sean's R1, 11:38-11:43 on 29 Oct 2026, and his direction at 11:43)
// =============================================================================================
//  His report, three times in one morning:
//
//      *"switching to another Link didn't connect, switching back didn't connect, and closing and
//      reopening the V2 window connected at once."*
//
//  And the direction:
//
//      *"make a Link switch and a click on the Chain tab do the same clean reset that reopening the
//      window does (release every session, lease and engage state, then engage the selected rack
//      fresh). Add a watchdog: if "Getting this rack ready / Connecting to rack" lasts more than
//      about 3 seconds, do that reset automatically and log why, with at most 2 tries, then a plain
//      sentence saying it couldn't connect and what to try."*
//
//  ---- WHY REOPENING THE WINDOW IS THE ONLY THING THAT WORKED ---------------------------------
//
//  The engage state is split between the processor and the EDITOR, and the editor's half is what a
//  reopen throws away. Three of its members can be left set by a path that returns early, and each
//  one blocks every later engage on its own:
//
//    borrowReadInFlightUid_   a rack read that never finished (bounded since 20 Oct)
//    borrowApplyInFlight_     an apply whose poll chain never reached borrowApplyFinish (UNBOUNDED
//                             until today - `if (p.borrowApplyInFlight_) return;` is the first
//                             thing the engage tick does, so a stranded apply is a permanent stall)
//    windowEngageAskedFor_    the one ask this window is allowed per rack, spent and never renewed
//
//  So the cure is not a hunt for the one path that stranded it - that hunt has been run three times
//  this month and found three different paths. The cure is that the state is RESETTABLE on a
//  gesture he already makes (a switch, a tab click) and that nothing can sit in it longer than a
//  few seconds without the plugin doing something about it.
//
//  ---- AND THE RESET IS NOT A RETRY LOOP -----------------------------------------------------
//
//  Two tries, then a sentence. A watchdog that keeps resetting would hide a real refusal behind a
//  spinner for ever, which is the fault it was written to end. After the second try the plugin says
//  it could not connect and what to try instead, and stops - a dead end that SAYS it is one.
// =============================================================================================

#include <JuceHeader.h>

namespace echojay
{

/** How long "Getting this rack ready / Connecting to rack" may stand before the watchdog acts. */
inline constexpr double kConnectStallMs = 3000.0;
/** And how many resets it may spend on it before it says so and stops. */
inline constexpr int kConnectResetTries = 2;
/** An apply whose ack never came. The poll chain itself gives up after 40 turns of 250 ms, so this
    is deliberately longer than that: it catches the chain not finishing, not the chain failing. */
inline constexpr double kApplyStallMs = 15000.0;

enum class ConnectAct
{
    Wait,      ///< nothing to do: not connecting, or not for long enough yet
    Reset,     ///< do the clean reset and engage again
    GiveUp     ///< two tries are spent: say it plainly and stop
};

struct ConnectStall
{
    ConnectAct   act = ConnectAct::Wait;
    juce::String say;          ///< the user-facing sentence (GiveUp only; empty otherwise)
    const char*  why = "nothing is trying to connect";
};

/** The watchdog's whole decision.

    `connectingNow` is a FACT, not the panel's words: the view is a Link's rack, no session is live
    on THAT rack, and something is nevertheless meant to be bringing one up (a pending engage, a
    held lock, a read or an apply in flight). The words come from that state, so a decision taken on
    the words would be a decision taken on a rendering.

    `msInState` is measured from when that became true, by a clock the caller keeps - a count of
    ticks would measure the tick rate instead (the "liveness in time, not counts" rule).

    `resetsDone` counts the resets this watchdog has already spent ON THIS RACK. It is reset when
    the rack connects or when he moves to another one, so two stalls an hour apart each get two
    tries rather than sharing them. */
inline ConnectStall connectStallAction (bool connectingNow,
                                        double msInState,
                                        int resetsDone,
                                        const juce::String& rackName,
                                        double thresholdMs = kConnectStallMs,
                                        int maxResets = kConnectResetTries)
{
    ConnectStall s;
    if (! connectingNow)
    {
        s.why = "nothing is trying to connect, so there is nothing to time out";
        return s;
    }
    if (msInState <= thresholdMs)
    {
        s.why = "it has not been trying for long enough to call it stuck";
        return s;
    }
    if (resetsDone >= maxResets)
    {
        s.act = ConnectAct::GiveUp;
        // ---- THE SENTENCE (no machinery, no em-dashes: ChainHost.h's own rule) --------------
        // It says what happened, and the one thing that has always worked for him.
        s.say = "Couldn't connect to " + (rackName.isNotEmpty() ? rackName : juce::String ("this rack"))
              + ". Close and reopen this window, or open that Link's own window to edit it there.";
        s.why = "the reset has been tried twice and it is still not connecting - saying so rather "
                "than spinning";
        return s;
    }
    s.act = ConnectAct::Reset;
    s.why = "it has been trying to connect for over three seconds - releasing every session, lease "
            "and engage state and engaging this rack fresh, which is what reopening the window does";
    return s;
}

/** The apply's own bound, in the same shape, so the two stalls read alike in the log. TRUE when an
    apply has been in flight long enough that it is not going to finish. */
inline bool applyHasStalled (bool applyInFlight, double msInFlight, double thresholdMs = kApplyStallMs)
{
    return applyInFlight && msInFlight > thresholdMs;
}

// =============================================================================================
//  AND WHAT THE MIRROR IS FOR: HIS R4, THE SAME MORNING
// =============================================================================================
//  *"an EchoJay Compressor on a Link's rack, opened in the V2 ... The gain reduction meter moves,
//  but no knob can be moved from the V2, and no message appears on the panel or the strip. So the
//  view follows the Link, but gestures go nowhere, silently."*
//
//  His log says why, 1463 times in ten minutes:
//
//      EJBorrow: the borrowed mirror of bb411b8240 holds 2 slot(s) while the rack's own sidecar
//      holds 1 - rendering the rack's own description, because the Link is the one publisher of it
//
//  The 16 Oct fix made the VIEW prefer the sidecar when the two disagree, which is right and is
//  what he sees drawn. But the MIRROR is what gestures act on - it holds the plugins, and bypass,
//  wet and every parameter write address it by index - and nothing re-read it. So the panel drew
//  the Link's rack while every gesture went to a mirror of a rack that no longer existed, and
//  landed on nothing.
//
//  A disagreement is therefore not a rendering detail. It means THIS SESSION'S MIRROR IS STALE,
//  and the only honest responses are to re-read it and, until that lands, to refuse gestures in
//  words. Papering over it in the renderer alone is what cost him this morning.
// =============================================================================================

struct MirrorAgreement
{
    bool         agree  = true;    ///< the mirror describes the rack the Link publishes
    bool         reread = false;   ///< the session's mirror must be read again
    juce::String say;              ///< what to tell him while that happens ("" = nothing to say)
    const char*  why = "there is no published rack to compare the mirror with";
};

/** Do the session's mirror and the Link's own published rack describe the same rack?

    NAMES IN ORDER, not just a count. His R4 case was a count disagreement, but the 28 Oct stale
    strip was a REPLACEMENT at the same count - and a gesture aimed at slot 1 of a mirror holding a
    different plugin is the same silent nothing either way. The names are what a slot IS to him.

    An EMPTY mirror with no published rack is not a disagreement: there is nothing to compare. */
inline MirrorAgreement mirrorVsRack (bool haveSidecar,
                                     const juce::StringArray& mirrorNames,
                                     const juce::StringArray& rackNames)
{
    MirrorAgreement a;
    if (! haveSidecar) return a;
    if (mirrorNames == rackNames)
    {
        a.why = "the mirror and the Link's own rack describe the same plugins in the same order";
        return a;
    }
    a.agree  = false;
    a.reread = true;
    a.why    = "the mirror and the Link's own rack disagree, so every gesture would address a rack "
               "that is no longer there";
    a.say    = "This rack changed on the Link. Reconnecting to it.";
    return a;
}

/** The sentence a gesture gets when it cannot be addressed at all.

    THERE WAS NO SENTENCE, which is the half of R4 he put first: *"no message appears on the panel
    or the strip"*, and *"a refusal must also never be silent"*. Every gesture road that gives up
    says one of these, so a dead knob is at worst an explained dead knob. */
inline juce::String gestureRefusalLine (const juce::String& what, const juce::String& rackName)
{
    const auto who = what.isNotEmpty() ? what : juce::String ("That control");
    return who + " didn't move: this view is reconnecting to "
         + (rackName.isNotEmpty() ? rackName : juce::String ("the Link"))
         + " after it changed. Try again in a moment.";
}

} // namespace echojay
