#pragma once
// BorrowStatusText — round C (17 Sep 2026): the borrowed-rack status line has
// ONE writer for the "changes write to it when you leave this rack" fact. Two
// writers composed it twice ("changes write to it when you leave this rack.
// ... Changes write to X when you leave this rack."). Pinned by
// tools/ui_round_guard (every flag combination -> exactly one occurrence).
#include <JuceHeader.h>

inline juce::String borrowStatusSentence (const juce::String& linkName, bool inContextOk, bool ctxCapable,
                                          bool restoredKept, const juce::String& withheldPrefix)
{
    juce::String s;
    s << withheldPrefix
      << "Editing " << linkName << " here - "
      << (inContextOk ? juce::String ("you're hearing the whole mix with your edits in place. ")
                      : ctxCapable ? juce::String ("your edits are heard here while you work. ")
                                   : linkName + "'s build can't hand the mix over - soloing your edit instead. Update it. ")
      << (restoredKept ? juce::String ("Your unwritten edits from the interrupted session were RESTORED. ") : juce::String())
      << "Changes write to " << linkName << " when you leave this rack. Solo this rack to hear it alone.";
    return s;
}
