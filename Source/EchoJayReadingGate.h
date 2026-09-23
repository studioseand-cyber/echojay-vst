// EchoJayReadingGate.h — 21p item 1 (23 Sep 2026): ONE validity gate for every reading the loop and the trim pass
// consume. Under the binding principle of 23 Sep ("subtle at every stage, loud only at the end") the loop never acts
// on an invalid reading: a −245 LUFS-S read is SILENCE, not a level.
//
// WHY A SHARED PREDICATE AND NOT A TEST AT EACH SITE. The loop already refused a quiet WINDOW (a −40 LUFS count
// floor on the chain output) while ChainHost::measureUnityTrims accepted any FINITE per-slot reading and wrote a
// trim of up to ±12 dB from it. Two tests, two opinions, one of them silent. This header is the only opinion:
// every consumer asks the same question and gets the same answer, and the answer carries its own reason so the
// refusal can be shown rather than inferred.
#pragma once
#include <JuceHeader.h>
#include <cmath>
namespace echojay
{
struct ReadingGate
{
    // A level below this is silence, not a quiet passage: the tally floors at −200 dB and a K-weighted silence
    // reads near −245, so anything under −60 cannot be a mix anyone is listening to.
    static constexpr float kFloorLufs = -60.0f;
    static constexpr float kFloorTruePeakDb = -60.0f;
    // A short-term window is 3 s; below this the figure is a partial window, not a measurement.
    static constexpr double kMinWindowSeconds = 3.0;

    bool valid = false;
    juce::String why;          // "" when valid; otherwise the reason, in the words the card shows

    static ReadingGate check (float shortTermDb, float truePeakDb, bool transportRolling, double heardSeconds)
    {
        ReadingGate g;
        if (! transportRolling)                              { g.why = "the transport was not rolling"; return g; }
        if (! std::isfinite (shortTermDb) || ! std::isfinite (truePeakDb)) { g.why = "the reading is not a number"; return g; }
        if (heardSeconds < kMinWindowSeconds)                { g.why = "less than " + juce::String (kMinWindowSeconds, 0) + " s heard"; return g; }
        if (shortTermDb <= kFloorLufs)                       { g.why = "silence (" + juce::String (shortTermDb, 1) + " LUFS-S, floor " + juce::String (kFloorLufs, 0) + ")"; return g; }
        if (truePeakDb  <= kFloorTruePeakDb)                 { g.why = "silence (" + juce::String (truePeakDb, 1) + " dBTP, floor " + juce::String (kFloorTruePeakDb, 0) + ")"; return g; }
        g.valid = true; return g;
    }
    /// The card / panel text for a figure that did not pass: an em dash, never a floor number dressed as a level.
    static juce::String noReadingText() { return juce::String::fromUTF8 ("\xe2\x80\x94"); }
};
} // namespace echojay
