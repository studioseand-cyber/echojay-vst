#pragma once

#include <string>

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

} // namespace echojay
