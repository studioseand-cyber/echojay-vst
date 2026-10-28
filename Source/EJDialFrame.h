#pragma once
#include <JuceHeader.h>

/** ---- A DIAL IS A STEP OF THE BUILD: THE DECISIONS (A1a, 28 Oct 2026) ------------------------

    B's two contracts, `CONTRACT_dial_step_2026-10-23.md` and `CONTRACT_dial_slot_2026-10-25.md`,
    turn a dial on playback from something the plugin does to itself after an add into a CALL the
    server makes and this client answers:

        { "op": "dial", "of": "<the add's id>" | "slot": 1, "name": "UnFairchild",
          "step_text": "Setting the UnFairchild... about 1 dB",
          "dial_on_playback": { ...the same object the add carries... } }

    Everything here is PURE and lives in a header so a guard can drive both directions of every
    decision, which the arming road itself cannot be made to do: it needs a hosted rack, a running
    transport and eight seconds of audio. The three decisions are the three ways this road went
    wrong in the contracts' own history:

      1. WHICH SLOT, and whether there is one at all. `of` names an add, so the slot is wherever
         that add LANDED - never the wire index it asked for, which is a stale pointer the moment a
         plugin fails to load (the remapBlocksToIdentity rule). `slot` names a slot that was
         already there, so the plugin standing in it must still be the one B named.
      2. WHAT THE STEP SAYS while it waits, because a card that holds the user on playback without
         saying what for is a card that looks stuck (his test 9, 22 Oct).
      3. WHAT THE ANSWER IS, including the two that are not outcomes at all (`skipped`) and the one
         that leaves the dial armed for the next playback (`not_heard`).                          */
namespace echojay::dialframe
{

/** Why a dial frame cannot run, in B's own words, or empty when it can. */
enum class Skip { None, AddFailed, SlotChanged, NoTarget };

inline const char* skipReason (Skip s) noexcept
{
    switch (s)
    {
        case Skip::AddFailed:   return "add_failed";
        case Skip::SlotChanged: return "slot_changed";
        case Skip::NoTarget:    return "no_target";
        case Skip::None:        break;
    }
    return "";
}

struct Target
{
    bool ok = false;
    int  slot1 = 0;            ///< 1-based, as the wire and the user count
    Skip skip = Skip::None;
    juce::String found;        ///< what IS in that slot, when the answer is slot_changed
};

/** The frame's slot, from whichever of the two roads it came down.

    `addLandedSlot1` is 0 when the add did not land (or there was no add), and `nameAtSlot` is what
    the rack says stands in `slot1` now - empty for an empty or unknown slot, which is itself a
    changed slot: B named a plugin and it is not there.

    NO SLOT AND NO ADD is `no_target` rather than a refusal, because the frame is well-formed and
    the thing it refers to is simply gone; a refusal would read to the model as "this client cannot
    dial", which is the opposite of what the capability says. */
inline Target dialFrameTarget (bool haveOf, bool addLanded, int addLandedSlot1,
                               bool haveSlot, int slot1,
                               const juce::String& nameAsked, const juce::String& nameAtSlot)
{
    Target t;
    if (haveOf)
    {
        if (! addLanded || addLandedSlot1 <= 0) { t.skip = Skip::AddFailed; return t; }
        t.ok = true; t.slot1 = addLandedSlot1; return t;
    }
    if (haveSlot)
    {
        if (slot1 <= 0) { t.skip = Skip::NoTarget; return t; }
        // IDENTITY, NOT PROSE: the name B sent came from this client's own inventory, so an exact
        // match (trimmed, case folded) is the whole test. A contains-match would make "Compressor"
        // dial the "EchoJay Compressor" standing in that slot, which is a different plugin.
        if (! nameAtSlot.trim().equalsIgnoreCase (nameAsked.trim()))
        { t.skip = Skip::SlotChanged; t.found = nameAtSlot.trim(); return t; }
        t.ok = true; t.slot1 = slot1; return t;
    }
    t.skip = Skip::NoTarget;
    return t;
}

/** The line the step shows. B sends `step_text`; the one thing it cannot know is whether anything
    is playing, and that is the whole reason the user is being held. */
inline juce::String dialStepLine (const juce::String& stepText, const juce::String& pluginName,
                                  bool transportKnown, bool playing)
{
    if (transportKnown && ! playing)
        return "Play the track to set the " + (pluginName.isNotEmpty() ? pluginName
                                                                      : juce::String ("plugin"));
    return stepText.trim().isNotEmpty() ? stepText.trim()
                                        : "Setting the " + pluginName + "...";
}

/** How long this road waits for playback before it answers `not_heard` and leaves the dial armed
    (B: "Wait up to 60 s for playback to start"). A ceiling, not a target. */
inline constexpr int kWaitForPlaybackMs = 60000;

enum class Outcome { Working, Reached, Short, Over, NotHeard };

inline const char* outcomeWord (Outcome o) noexcept
{
    switch (o)
    {
        case Outcome::Reached:  return "reached";
        case Outcome::Short:    return "short";
        case Outcome::Over:     return "over";
        case Outcome::NotHeard: return "not_heard";
        case Outcome::Working:  break;
    }
    return "";
}

/** THE ANSWER'S TIMING, WHICH IS THE WHOLE DEFECT B'S CONTRACT FIXES. Sean's test 3: the closing
    arrived straight after the adds and the dial never reported. So this road answers when, and only
    when, one of four things is true:

      - the dial has landed (the rack published a result for this plugin)        -> its outcome;
      - the dial gave up at an end of its control                               -> short / over;
      - nothing played for kWaitForPlaybackMs                                   -> not_heard;
      - otherwise it is still working, and the step stays open.

    `landedWord` is what the rack's own result says (the dial writes "reached", "short" or "over" -
    one vocabulary, B's, from the figure that was measured); an unrecognised word is treated as
    REACHED WITH NO FIGURE rather than as still working, because a dial that has published its
    result is finished whatever it called it, and holding the step open on an unknown word is how a
    run hangs. */
inline Outcome dialAnswerNow (bool landed, const juce::String& landedWord,
                              bool heardAnything, double waitedMs)
{
    if (landed)
    {
        const auto w = landedWord.trim().toLowerCase();
        if (w == "short") return Outcome::Short;
        if (w == "over")  return Outcome::Over;
        return Outcome::Reached;
    }
    if (! heardAnything && waitedMs >= (double) kWaitForPlaybackMs) return Outcome::NotHeard;
    return Outcome::Working;
}

} // namespace echojay::dialframe
