#pragma once

#include <JuceHeader.h>

// ===========================================================================
// THE LINK IS THE PROCESSOR; THE V2 IS A REMOTE CONTROL (10 Oct 2026)
// ===========================================================================
//
// SEAN'S COMPLAINT, AND WHY IT WAS NOT A BUG IN ANY ONE PLACE. Selecting a
// Link in the V2 took a LEASE on its rack, and a lease did three things
// (LinkProcessor::rackLeaseEngage): it forced every slot's effective bypass
// true, saved the user's intent separately, and streamed the channel dry.
// The V2 then hosted a COPY of that rack and mixed its output into the V2's
// own buffer. So during a lease:
//
//   kick channel, at the Link ............ the kick, UNPROCESSED
//   kick fader / pan / sends ............. the kick, UNPROCESSED
//   the drum bus and its chain ........... the kick, UNPROCESSED
//   the V2's position (e.g. mix bus) ..... the kick, PROCESSED, summed in
//
// The processed kick bypassed its own channel strip and its own bus. Nothing
// about that was accidental - it is exactly what the lease was designed to do -
// which is why it could not be fixed by correcting a line. The design had to
// change: THE LINK ALWAYS PROCESSES ITS OWN RACK, in its own place in the
// host's graph, and selecting a Link stops being an audio act at all. It
// becomes "which Link am I addressing".
//
// WHAT STAYS: the lease as a CONTROL concept - the command channel, the acks,
// the structure plans, the sidecar, the meter frames, the staleness guards.
// Those are how the V2 addresses a rack, and they are the whole remote
// control. What goes is the AUDIO DETOUR.
//
// WHY A PREDICATE AND NOT A DELETION, YET. The plan (docs/LINK_REMOTE_CONTROL_PLAN.md
// section 8) puts the removals LAST, at stage 5, "because the removals delete
// the paths the earlier stages are still being compared against". It is right:
// the detour's machinery - the alignment lines, the pad arithmetic, the mute
// confirmation, the solo fabric's honorary strip - is the only existing
// description of what correct monitoring looked like, and deleting it in the
// same change that disables it would leave nothing to compare a regression
// against. So stage 1 is one named seam, provable both directions, and the
// dead paths come out at stage 5 when stages 1-4 are green.
//
// It also lives in a header the guards compile, for the reason recorded in
// EJUserDataWrite.h (open list 217): the gate links harnesses and never the
// processors, so a policy expressed only as an `if` inside processBlock is a
// string a pin can grep for. cmpMixTargetGain is the proof it matters - it was
// an expression inside processBlock, which is how an A/B regression survived
// 3,858 green checks.

namespace echojay
{

/** STAGE 1 (10 Oct 2026): MAY A BORROWED RACK'S AUDIO TAKE A DETOUR THROUGH
    THE V2?

    No. The Link processes its own rack in its own place, so there is nothing
    for the V2 to stand in for. Both audio consumers of the borrow read this:

      - the IN-CONTEXT INJECTION (`ctxNow`), which mixed the borrowed host's
        output into the V2's buffer;
      - the THROUGH-MAIN MONITORING FALLBACK (`fallbackSolo`), which routed the
        borrowed channel through the V2 when in-context was refused.

    Both existed to make an edited channel audible while its own rack was
    switched off. Its rack is no longer switched off, so both are not merely
    unnecessary - they are a SECOND copy of a signal that is already audible
    where it belongs. Leaving either on would be the double-processing the plan
    names as stage 1's failure mode.

    `borrowActive()` deliberately still answers true: the V2 is still
    addressing that rack, the sidecar still publishes, the acks still flow.
    This predicate separates "I am addressing this rack" from "its audio comes
    through me", which the old code conflated in one flag. */
inline bool borrowAudioDetourAllowed() noexcept { return false; }

/** STAGE 1: DOES TAKING A RACK LEASE BYPASS THE LINK'S OWN SLOTS?

    No. This is the other half of the same change and it is the half Sean can
    hear: `rackLeaseEngage` called setAttachBypassed(true) and setLeaseBypass(i,
    true) for every slot, so the rack went dry in place and the channel strip
    downstream of it carried an unprocessed signal.

    The user's INTENT (`intendedBypassed`) was always stored separately and is
    untouched by this change - which is why the rack simply plays now, and why
    a release becomes an idempotent restatement of that intent rather than a
    restore from a snapshot. Four bugs retire with the bypassing, all of them
    real and all from the week before this change: the persistent bypass after
    a hand-back, the bypass DISPLAY reading bypassed on every leased slot, the
    parked-rack notices, and the "rack has 0 slots" refusals that came from a
    Link whose slots were parked. */
inline bool leaseBypassesTheRack() noexcept { return false; }

/** STAGE 1: DOES A RACK LEASE MUTE THE LINK'S OUTPUT?

    No, and this is the half that would have shipped something WORSE than the
    bug it was fixing. The lease composed a third mute reason
    (`rackLeaseActive_ && rackLeaseMuteWant_`), and it was right to: the V2 was
    summing a processed copy of this channel into its own output, so leaving
    the Link audible as well would have been an obvious double. The mute was
    the detour's other half.

    With no injection there is nothing to avoid doubling, and a lease that
    still muted would make SELECTING A LINK SILENCE THAT CHANNEL. The original
    complaint was a kick that bypassed its own bus; this would have been a kick
    that bypassed its own existence.

    I did not reason my way to this - linksync_test's mute/solo legs failed with
    a peak of 0 the moment the rack stopped being bypassed, which is the whole
    reason those legs assert on rendered samples instead of on flags.

    THE USER'S OWN MUTE AND THE SOLO FABRIC ARE UNTOUCHED. Only the LEASE arm
    of the composition goes. muteUserOn_ is an act of the user's and
    soloMuteWant_ belongs to the solo set; neither was ever about the detour.
    The V2 should also stop ASKING for the mute, but the Link refuses it here
    as well, because an older V2 on the other end of the channel will still
    send `muteOut` and must not be able to silence a channel with it. */
inline bool leaseMutesTheLink() noexcept { return false; }

// ---- STAGE 4 (10 Oct 2026): EDITORS -----------------------------------------------
//
// AN EDITOR BELONGS TO THE INSTANCE THAT OWNS IT. The V2 cannot show a window for a
// plugin living in another process, so `open_editor` asks the LINK to open its own
// floating window for slot N. The knobs are then the real instance's and a turn is
// instant by construction - it works in every host and needs no shared address
// space.
//
// `embed` IS THE NICER ONE AND IT IS NOT ALWAYS SAFE. In Logic today both plugins do
// share a process - every one of the six AUHostingServiceXPC crash reports from
// 8 October lists EchoJay V2 and EchoJay Link in one process's usedImages. That is a
// fact about Logic's AU view hosting, NOT a guarantee: AUv3 and sandboxed hosts put
// each plugin in its own container, Bitwig sandboxes by default, Reaper can bridge
// selectively, and AAX has its own rules.
//
// SO IT IS PROVEN AT RUNTIME, NEVER INFERRED FROM THE HOST'S NAME. The rack sidecar
// already publishes `publisherPid` - the pid of the process writing it - so "are we
// in the same process as this Link" is one integer comparison against our own
// getpid(). Cheap, honest, and it cannot be fooled by a host that renames itself or
// by a version of Logic that changes its mind about view hosting.

/** Where an editor may be put. `Float` is the default and the only one promised. */
enum class EditorWhere { Float, Embed };

/** May `embed` be honoured for a Link whose sidecar says it is published by
    `linkPublisherPid`, when we are `ourPid`?

    Only on a POSITIVE match. A sidecar with no pid (0, an older Link that does not
    publish one) is NOT a match: absence is not permission, and treating it as one
    would embed across a process boundary in exactly the hosts that sandbox. The
    refusal is then reported with its reason, because "embed quietly became float" is
    the kind of silent downgrade that gets discovered months later in a bug report
    about latency that was never about latency. */
inline bool embedAllowed (int linkPublisherPid, int ourPid) noexcept
{
    return linkPublisherPid > 0 && ourPid > 0 && linkPublisherPid == ourPid;
}

/** The sentence a refused `embed` carries back. One author, so the ack, the log and
    anything a model is shown all say the same thing. */
inline juce::String embedRefusedReason (int linkPublisherPid, int ourPid)
{
    if (linkPublisherPid <= 0)
        return "this Link does not publish a process id, so the same-process proof cannot be made - "
               "opened as a floating window instead";
    if (linkPublisherPid != ourPid)
        return "this Link runs in another process (pid " + juce::String (linkPublisherPid)
             + ", this is " + juce::String (ourPid) + "), so its editor cannot be embedded here - "
               "opened as a floating window instead";
    return {};
}

} // namespace echojay
