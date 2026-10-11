#pragma once
// BorrowStatusText — round C (17 Sep 2026): the borrowed-rack status line has
// ONE writer for the "when changes reach the Link" fact. Two writers composed
// it twice ("changes write to it when you leave this rack. ... Changes write
// to X when you leave this rack."). Pinned by tools/ui_round_guard.
//
// ---- 11 Oct 2026, THE REMOVALS: WHAT THIS LINE SAYS, AND WHY IT CHANGED ----
//
// It used to open by telling the user how they were HEARING the rack they were
// editing, because that was a real question with three answers: in-context
// (the mix with your edits summed in at this plugin's position), heard-here
// (the V2 playing the edited copy on its own), or refused with a reason ("X's
// build can't hand the mix over - soloing your edit instead. Update it.").
//
// There is one answer now. The Link processes its own rack, in its own place,
// so the user hears their edits through that channel's fader, its sends and
// its bus - which is what they were asking for when they complained about
// balancing a kick that was not going through its drum bus. So `inContextOk`
// and `ctxCapable` are gone: there is nothing left to be capable OF.
//
// The second half had to change with it and is the more careful claim, because
// it is now TWO facts rather than one. The RACK reaches the Link as the user
// edits it: every add, remove, move, bypass and value is a command, sent and
// acked when it is made. A hosted plugin's OWN settings are still committed
// when the rack is left, because the V2 holds a mirror of that plugin and the
// state goes with the hand-back. Saying "changes write when you leave" would
// now be wrong about the rack; saying "changes are live" would be wrong about
// the settings. It says both.
inline juce::String borrowStatusSentence (const juce::String& linkName,
                                          bool restoredKept, const juce::String& withheldPrefix)
{
    juce::String s;
    s << withheldPrefix
      << "Editing " << linkName << " here - you're hearing it in its own place in the mix, through its "
      << "own fader and bus. "
      << (restoredKept ? juce::String ("Your unwritten edits from the interrupted session were RESTORED. ") : juce::String())
      << "Rack changes reach " << linkName << " as you make them; plugin settings are written when you "
      << "leave this rack. Solo this rack to hear it alone.";
    return s;
}
