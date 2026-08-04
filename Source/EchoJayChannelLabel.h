#pragma once

#include <algorithm>
#include <string>
#include <vector>

// ============================================================================
// Channel identity from a Link label — ONE predicate, two idioms.
//
// channelDisplayLabel(uid) degrades: live name -> the chat's stored snapshot
// -> THE RAW UID. That last step is marked "never expected" at its definition,
// but it is a real return value for a Link that has vanished or was never
// named, and every consumer has to reject it.
//
// Two consumers did not agree about that. The prose composer guarded it
// (PluginEditor.cpp, the [TARGET CHANNEL] block: `label.isEmpty() || label ==
// targetLinkUid`), and materialContextName did not — so the same unnamed Link
// produced careful prose on one path and a 12-character hex string as CHANNEL
// on the other. That string reaches /api/classify and the system prompt's
// CHANNEL TYPE.
//
// WHY THE HEX STRING IS WORSE THAN NOTHING. The classify prompt's fail-safe is
// "If CHANNEL is unknown, set precondition to null. Never guess" — a rule that
// only fires when the channel is ABSENT. A uid is present and meaningless, so
// the fail-safe never engages and the model reasons about a channel that does
// not exist. Absent is a state the system handles; wrong is not.
//
// The predicate lives here, JUCE-free and tested under plain g++, so the two
// sites cannot drift apart again by comment rot. They deliberately DIFFER in
// what they substitute — prose wants a phrase, a field wants emptiness — and
// that is exactly why the shared thing is the TEST and not the replacement.
// ============================================================================

namespace echojay
{

// Is this label real, or is it channelDisplayLabel's uid passthrough / empty?
inline bool channelLabelUsable (const std::string& uid, const std::string& label)
{
    return ! label.empty() && label != uid;
}

// The material-context decision, whole, so both branches are fixture-able.
//
//   main chat      (no linkUid)        -> the processor's own channel
//   channel chat   (label resolved)    -> that Link's display name
//   channel chat   (label unresolved)  -> EMPTY, meaning UNKNOWN
//
// Empty is a deliberate third answer, not a failure to produce one. It must
// NOT fall back to the processor's channel: this conversation is not on that
// channel, and saying so would reintroduce the disagreement between what the
// conversation is and what the model is told it is. Each consumer expresses
// unknown in its own idiom — the classify body omits the field entirely (so
// the prompt's fail-safe engages and no precondition is set), and the system
// prompt omits the CHANNEL TYPE block rather than emitting `CHANNEL TYPE: ""`.
inline std::string resolveMaterialContext (const std::string& chatLinkUid,
                                           const std::string& linkLabel,
                                           const std::string& mainDefault)
{
    if (chatLinkUid.empty())                             return mainDefault;
    if (! channelLabelUsable (chatLinkUid, linkLabel))   return {};
    return linkLabel;
}

// ----------------------------------------------------------------------------
// Switch-destination ordering: MEMBERSHIP FROM THE REGISTRY, ORDER FROM THE
// MODEL. The two are separate powers on purpose.
//
// registryUids is the canonical Link list (getLinkDisplayList order), which is
// the ONLY thing that decides who appears. It includes Links the classifier
// never saw: the server drops nameless links from LINKS entirely, so an
// unnamed track is unrankable but still a perfectly good destination.
//
// rankedUids is the model's opinion, and it is only ever an opinion. Ids it
// invents are skipped rather than trusted — a Link can also legitimately
// vanish between the classify call and the tap — so a wholly wrong ranking
// degrades to the canonical order and costs the user nothing.
//
// hereUid is dropped: switching to the channel you are on is a no-op wearing
// a menu row.
inline std::vector<std::string> orderSwitchDestinations (
    const std::vector<std::string>& registryUids,
    const std::vector<std::string>& rankedUids,
    const std::string& hereUid)
{
    std::vector<std::string> out;
    const auto eligible = [&] (const std::string& uid)
    {
        return ! uid.empty() && uid != hereUid
            && std::find (registryUids.begin(), registryUids.end(), uid) != registryUids.end()
            && std::find (out.begin(), out.end(), uid) == out.end();
    };

    for (const auto& uid : rankedUids)      // the model's order, validated
        if (eligible (uid)) out.push_back (uid);
    for (const auto& uid : registryUids)    // everyone else, canonical order
        if (eligible (uid)) out.push_back (uid);

    return out;
}

} // namespace echojay
