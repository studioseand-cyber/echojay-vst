#pragma once
// =============================================================================================
//  EJPlaybackHeard.h - WHERE "NOTHING PLAYED" IS MEASURED, AND WHY IT WAS THE WRONG PLACE
//  (Sean's 11:30 report of 29 Oct 2026, item 1)
// =============================================================================================
//  His words:
//
//      *"EchoJay added Shadow Hills Mastering Compressor to the Mix Bus, and the slot meter showed
//      IN -15.7, OUT -43.6 (about -28 dB) ... "wait for playback" said "Nothing played in 60
//      seconds" and the second wait sat at "Listening... 0 of 8 s" while Sean was playing ... check
//      whether the listener measures after the slot (so a 28 dB drop reads as silence) and make it
//      measure the input or say "the level after this plugin is very low".*
//
//  ---- IT DOES, AND THE ARITHMETIC IS EXACT --------------------------------------------------
//
//  The listen counts `heardAboveSeconds` on the CHAIN OUT tally, and that counter only counts a
//  100 ms hop whose own level is above the loudness loop's counting floor, which is
//  LoudnessLoop::kCountFloorLufs = -40 LUFS. His rack's output sat at -43.6 dB. So every hop was
//  below the floor, nothing was counted, "Listening... 0 of 8 s" was a true statement about the
//  wrong signal, and sixty seconds later the wait said nothing had played while he was playing.
//
//  B has since found the cause of the drop itself - the profile's neutral write did not run on
//  that add, so the plugin's Optical Gain went in at 1 of 1..24 - so the level was real. That does
//  not excuse the measurement: a plugin set wrong, or simply turned down, must not be able to make
//  "nothing played" true. What played is a fact about the INPUT.
//
//  ---- SO THE WAIT ASKS THE INPUT, AND THE OUTPUT BECOMES A SEPARATE SENTENCE ----------------
//
//  Two different questions, which this file keeps apart:
//
//    1. HAS HE PLAYED ANYTHING?        answered at the rack's input, which is what reaches the
//                                      plugins, and which no plugin can turn down.
//    2. IS ANYTHING COMING OUT?        answered at the output, and when the answer is "far less
//                                      than went in", that is worth saying out loud rather than
//                                      waiting out a timeout on.
//
//  A Link answers (1) with its own frame, which has no gated input count: there the output figure
//  is all there is, and the decision says so rather than pretending otherwise. The slot-level
//  check in EJSlotSwallow.h is what covers that case, because a Link publishes per-slot in and out.
// =============================================================================================

#include <JuceHeader.h>

namespace echojay
{

/** The loudness loop's counting floor, by value rather than by include: this header is pure and
    must not drag LoudnessLoop.h (and the DSP tree behind it) into every harness that holds the
    rule. A leg asserts the two are the same number, which is the only way a copy stays honest. */
inline constexpr float kHeardCountFloorLufs = -40.0f;

/** How far below its input a rack's output may sit before it is worth a sentence. Twenty dB is not
    a tone control or a trim; it is a plugin that is not passing the programme. */
inline constexpr float kOutputFarBelowDb = 20.0f;

/** WHICH figure answers "has anything played", and where it came from. */
struct PlaybackHeard
{
    float        seconds = 0.0f;   ///< the gated seconds the wait should count
    bool         fromInput = false;///< true = measured at the rack's input (the honest place)
    const char*  why = "";
};

/** `inputKnown` is false for a rack this instance does not own (a Link's frame carries no gated
    input count), and then the output figure is all there is - which the reason says, so a log can
    tell a measured input from a fallback. */
inline PlaybackHeard playbackHeard (bool inputKnown, float inputSeconds, float outputSeconds)
{
    PlaybackHeard h;
    if (inputKnown)
    {
        h.seconds   = juce::jmax (0.0f, inputSeconds);
        h.fromInput = true;
        h.why = "counted at the rack's input, which is what reaches the plugins - a plugin that is "
                "turned down cannot make \"nothing played\" true";
        return h;
    }
    h.seconds = juce::jmax (0.0f, outputSeconds);
    h.why = "counted after the rack, because this rack is a Link's and its frame carries no input "
            "count - the slot check is what notices a plugin swallowing the signal there";
    return h;
}

/** AND THE OTHER QUESTION, asked of the two levels rather than of the counters.

    TRUE when the input is clearly playing and the output is both far below it AND under the floor
    the listen counts against - which together are the state his 11:30 report was sitting in. Both
    halves matter: a 20 dB drop that still leaves -25 dB out is a loud plugin doing its job and the
    listen succeeds anyway, so it is not worth a sentence. */
struct OutputFarBelow
{
    bool         is = false;
    float        dropDb = 0.0f;
    juce::String say;
};

inline OutputFarBelow outputFarBelow (bool levelsKnown, float inDb, float outDb,
                                      float floorDb = kHeardCountFloorLufs,
                                      float farDb   = kOutputFarBelowDb)
{
    OutputFarBelow o;
    if (! levelsKnown) return o;
    if (! (std::isfinite (inDb) && std::isfinite (outDb))) return o;
    if (inDb <= floorDb) return o;                  // nothing is playing in: not this fault
    o.dropDb = inDb - outDb;
    if (o.dropDb < farDb || outDb > floorDb) return o;
    o.is = true;
    // No machinery and no em-dashes (ChainHost.h's own rule), and it names both figures because a
    // sentence about a level he cannot see is not actionable.
    o.say = "The level after this rack is very low: " + juce::String (inDb, 1) + " dB in, "
          + juce::String (outDb, 1) + " dB out. Check the plugin that is pulling it down, or bypass "
            "it, then ask me again.";
    return o;
}

} // namespace echojay
