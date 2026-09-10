#pragma once

// ===========================================================================
// CAPTURE GUARD: OUTPUT SUBSTITUTION (10 Sep 2026)
//
// THE DEFECT THIS CLOSES. Three separate features replace EchoJay's output
// buffer with audio read from a file, and all three do it UPSTREAM of the
// taps that feed the meters and the capture engine:
//
//   A/B playback        PluginProcessor.cpp ~:714   assigns into the buffer
//   compare streams     PluginProcessor.cpp ~:848   crossfades into it
//   codec preview       a compare stream carrying a decoded lossy render
//
// The taps are chainHost.process, then meterEngine.processBlock, then
// captureEngine.processBlock. So a capture taken while any of them is running
// measures the substituted audio and records it as the user's mix: every
// figure, the key pass, the figure card, and the chat injection. Nothing
// downstream can tell it apart from a real capture, because until now nothing
// wrote down that it happened.
//
// startCapture guarded ONE thing before this: editActive(). It knew nothing
// about the other three.
//
// WHY A STATE STRUCT AND NOT A METHOD ON THE PROCESSOR. The predicate is the
// part worth pinning, and the gate cannot construct an EchoJayProcessor. The
// state travels as plain bools so tools/mapfps_test drives the SHIPPED
// function rather than a copy of its reasoning; the processor's job is only
// to fill the struct from its atomics.
//
// Header-only inline on purpose, in the manner of EJDialWrites.h and
// EJMisdialReport.h: including it IS the implementation.
// ===========================================================================

#include <JuceHeader.h>

namespace echojay
{

/** Which feature is replacing the plugin's output, if any. */
enum class OutputSubstitution { None = 0, ABPlayback, ComparePlayback, CodecPreview };

/** The processor's audio-path state, flattened so the predicate is testable.
    Every field mirrors an atomic on EchoJayProcessor; monGain is deliberately
    ABSENT, see substitutionActive below. */
struct OutputSubstitutionState
{
    bool abActive         = false;      // a file is loaded into the A/B player
    bool abPlayingRef     = false;      // ...and it is outputting, not passing through
    bool codecPreview     = false;      // the compare slots hold a codec render
    int  cmpAudible       = -1;         // which compare stream is audible, -1 = none
    bool cmpLoaded[2]     = { false, false };
    bool cmpPlaying[2]    = { false, false };
    bool cmpStopAtZero[2] = { false, false };   // fading out to disengage
};

/** True when the audible compare stream is actually replacing output.
    THREE CONDITIONS, ALL LOAD-BEARING, and each is a way to be wrong:
      loaded      a stream can be selected with no buffer behind it
      playing     a loaded, parked stream contributes nothing
      !stopAtZero a stream mid-disengage is on its way out, not in

    A stream that is playing but NOT audible is analysed into its own meter
    and never reaches the output buffer, which is why this keys on cmpAudible
    rather than on either stream's playing flag.

    NOT COVERED: the ~8ms monitor fade tail after stopAtZero, because monGain
    is audio-thread-only state and reading it from the message thread would be
    a race. A capture started inside that window would carry up to 8ms of
    fading substituted audio at the head and be recorded as clean. Named here
    rather than papered over; it is not worth an atomic. */
inline bool compareStreamSubstituting (const OutputSubstitutionState& s) noexcept
{
    const int a = s.cmpAudible;
    if (a < 0 || a > 1) return false;
    return s.cmpLoaded[(size_t) a] && s.cmpPlaying[(size_t) a] && ! s.cmpStopAtZero[(size_t) a];
}

/** Which substitution is active, most specific first.

    PRECEDENCE FOLLOWS THE AUDIO PATH, not a preference. A/B assigns into the
    buffer first and the compare crossfade then writes over it, so when both
    are running what leaves the plugin is the compare stream and naming A/B
    would send the user to stop the wrong thing.

    Codec preview is a compare stream, so it requires one to be substituting:
    the flag alone, with the streams parked, replaces nothing. */
inline OutputSubstitution activeOutputSubstitution (const OutputSubstitutionState& s) noexcept
{
    const bool cmp = compareStreamSubstituting (s);
    if (cmp && s.codecPreview)        return OutputSubstitution::CodecPreview;
    if (cmp)                          return OutputSubstitution::ComparePlayback;
    if (s.abActive && s.abPlayingRef) return OutputSubstitution::ABPlayback;
    return OutputSubstitution::None;
}

/** The token stored on the capture record. "" = none, per the metering
    convention that an absent key means unavailable rather than zero. */
inline const char* outputSubstitutionKey (OutputSubstitution s) noexcept
{
    switch (s)
    {
        case OutputSubstitution::ABPlayback:      return "ab";
        case OutputSubstitution::ComparePlayback: return "compare";
        case OutputSubstitution::CodecPreview:    return "codec";
        case OutputSubstitution::None:            break;
    }
    return "";
}

/** What the user is told, and what to do about it. Each names the feature
    that is running, because "capture is unavailable" sends someone hunting.
    Empty for None: a refusal with no cause is not a refusal. */
inline juce::String captureRefusalReason (OutputSubstitution s)
{
    switch (s)
    {
        case OutputSubstitution::CodecPreview:
            return "Codec preview is playing through EchoJay's output, so a capture "
                   "would measure the codec render instead of your mix. Close the "
                   "codec panel, then capture.";
        case OutputSubstitution::ComparePlayback:
            return "Compare is playing a stored file through EchoJay's output, so a "
                   "capture would measure that file instead of your mix. Stop Compare "
                   "playback, then capture.";
        case OutputSubstitution::ABPlayback:
            return "A stored capture is playing back through EchoJay's output, so a "
                   "capture would measure that playback instead of your mix. Stop the "
                   "playback, then capture.";
        case OutputSubstitution::None:
            break;
    }
    return {};
}

// ---------------------------------------------------------------------------
// The capture record's field.
//
// WRITTEN FOR A FUTURE THE GUARD CURRENTLY FORBIDS. With the guard in place
// this is "" on every capture that exists, because a capture cannot start
// while anything is substituting. It is here because section 4 of the compare
// plan wants Playback Simulation captures taken THROUGH a simulation on
// purpose, and the moment that guard is relaxed the field starts carrying its
// value with no further plumbing. Adding it afterwards is how the same defect
// arrives a second time: a measurement that cannot say what it measured.
//
// Absent key = no substitution, so an ordinary capture adds nothing to the
// state blob and every capture written before today reads back correctly.
// ---------------------------------------------------------------------------

inline constexpr const char* kCaptureSubstitutionKey = "outputSubstitution";

/** Writes nothing when there was no substitution: see the absent-key rule. */
inline void writeCaptureSubstitution (juce::DynamicObject& o, const juce::String& sub)
{
    if (sub.isNotEmpty())
        o.setProperty (juce::Identifier (kCaptureSubstitutionKey), sub);
}

/** An UNKNOWN token comes back verbatim rather than blanked. A later build
    may write a token this one has never heard of, and silently dropping it
    would turn a labelled capture into an unlabelled one, which is the exact
    failure the field exists to prevent. */
inline juce::String readCaptureSubstitution (const juce::DynamicObject& o)
{
    const juce::Identifier id (kCaptureSubstitutionKey);
    return o.hasProperty (id) ? o.getProperty (id).toString() : juce::String();
}

} // namespace echojay
