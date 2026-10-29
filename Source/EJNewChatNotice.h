#pragma once
#include <JuceHeader.h>

/** ---- WHAT A NEW CHAT SAYS ABOUT THE RUN IT STOPPED (Sean's ruling, 28 Oct 2026) ------------

    Two of his rulings meet on the New chat button and only one of them was implemented:

      17 Oct, item 1 — *"a new chat never inherits the last one's run"*. So pressing New chat stops
      anything still waiting, which the code has done since then.

      23 Oct, 21:06 — *"Before a new run supersedes a live one, the plugin asks … **Never cancel a
      run silently**."*  That ask belongs to the TYPED-MESSAGE gesture; New chat is a different
      gesture, and it was stopping a live run with **nothing said to the person**. There was a log
      line, and a log line is not telling him.

    His ruling of 28 Oct, after that was raised: **option 1 — the new chat's first line says the run
    was stopped.** So the stop stays instant (no question in front of a gesture that had none), and
    it stops being silent.

    PURE AND HERE so both directions can be driven by a guard: what it says, and when it says
    nothing at all. The caller owns WHERE it is drawn (the new chat's own first line) and the
    editor's own stop site owns WHEN it is called - this file owns only the sentence, which is the
    part a reader will quote back.                                                                */
namespace echojay
{

/** The line a new chat opens with when it stopped a run, or EMPTY when it stopped nothing.

    `target` is the Link or channel the run was working on, as the user knows it (the editor passes
    `channelDisplayLabel`). Empty target is the V2's own rack, and the sentence then names no
    channel rather than inventing one - "the run" is honest, "the run on " with nothing after it is
    not.

    `wasLive` is the one fact that decides whether anything is said: it is the same condition the
    stop itself is made on (`isActive() || askPending()`), passed in rather than re-derived, because
    a sentence that decides for itself whether a run existed is a second answer to that question. */
inline juce::String newChatStoppedLine (bool wasLive, const juce::String& target)
{
    if (! wasLive) return {};
    const auto t = target.trim();
    return t.isNotEmpty()
             ? "A new chat doesn't carry the last one's run, so the run on " + t + " was stopped."
             : "A new chat doesn't carry the last one's run, so the run that was going was stopped.";
}

} // namespace echojay
