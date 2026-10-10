#pragma once

// ===========================================================================
// MULTI-CHANNEL CAPTURE: the tally, the header, the markers and the ONE
// instruction (9 Sep 2026).
//
// WHAT WAS WRONG. The payload wrote a full paragraph PER CHANNEL for every
// channel that was silent, each one ending "You may say the channel was
// silent; that is a real measurement, not a guess." On a real multitrack over
// a one-minute window a dozen channels are normally not playing, so the model
// received a dozen explicit licences to say so and used all of them. The user
// read a fault report about normal material. The model was complying, not
// hallucinating, which is why no prompt rule could have suppressed it: the
// instruction it was following was in the payload.
//
// SILENCE IS NORMAL AND NO-FRAMES IS NOT, and the two must never merge. They
// are different facts with opposite standing:
//   SILENT     frames arrived and the level stayed at or below the floor for
//              the whole window. A real measurement. Ordinary on a multitrack.
//   NO FRAMES  nothing arrived from that Link. The cause is UNKNOWN and it is
//              never a claim about sound. This is the one that can mean a
//              broken capture.
// The original code already drew this line correctly and then buried it under
// two paragraphs of equal weight. The markers keep the distinction and the
// guidance carries the meaning once.
//
// THE NO-FRAMES ADVICE SCALES WITH THE COUNT, and it is computed here rather
// than left to the model. One or two channels missing is a note beside an
// otherwise good capture. Most of them missing is a failed capture, and only
// then should the reply ask for another. A rule stated as a rule gets applied
// unevenly; a sentence chosen from the actual number does not.
//
// Header-inline so the gate compiles the SHIPPED text rather than a copy, in
// the same manner as EJDialTally.h and EJRefusalLine.h. PluginEditor.cpp holds
// no wording of its own for any of this.
// ===========================================================================

#include <JuceHeader.h>
#include <vector>

namespace echojay
{

// One channel's outcome, reduced to what the payload actually decides on.
// Mirrors ChannelMeterData without depending on it, so the gate can build a
// forty-channel session without a capture.
struct CaptureChannelOutcome
{
    bool        isHost         = false;
    juce::int64 framesReceived = -1;    // -1 host/na, 0 none arrived, >0 real
    bool        silentFloor    = false; // every level at or below the floor

    bool noFrames() const noexcept { return ! isHost && framesReceived == 0; }
    bool silent()   const noexcept { return framesReceived > 0 && silentFloor; }
};

struct MultiChannelTally
{
    int total    = 0;   // every channel in the capture, host included
    int links    = 0;   // the Links only, the denominator for "most of them"
    int silent   = 0;
    int noFrames = 0;
};

inline MultiChannelTally tallyCaptureChannels (const std::vector<CaptureChannelOutcome>& chs)
{
    MultiChannelTally t;
    t.total = (int) chs.size();
    for (const auto& c : chs)
    {
        if (! c.isHost)   ++t.links;
        if (c.silent())   ++t.silent;
        if (c.noFrames()) ++t.noFrames;
    }
    return t;
}

// The header line. The counts are here so a reply can say "nine of forty"
// without the model counting markers for itself, which it would do wrong.
inline juce::String multiChannelHeader (const MultiChannelTally& t)
{
    juce::String h;
    h << "\n\n[MULTI-CHANNEL CAPTURE - " << t.total << " channels, "
      << t.silent << " silent, " << t.noFrames << " with no frames]\n";
    return h;
}

// The per-channel line. Nullptr means the numbers follow as usual.
inline const char* captureChannelMarker (const CaptureChannelOutcome& c) noexcept
{
    if (c.noFrames()) return "NO FRAMES\n";
    if (c.silent())   return "SILENT\n";
    return nullptr;
}

// The ONE instruction, above the channel list, replacing the per-channel
// licences. Each half appears only when that outcome actually occurred, so an
// ordinary capture with nothing silent pays nothing for either.
inline juce::String multiChannelGuidance (const MultiChannelTally& t)
{
    juce::String g;

    if (t.silent > 0)
    {
        g << "SILENT marks a channel that WAS receiving audio and whose level stayed "
             "at or below the silence floor for the whole window. That is a real "
             "measurement, not a fault, and over a short window a multitrack normally "
             "has several channels not playing. "
          << t.silent << " of " << t.total << " channels are marked SILENT. Report "
             "them by COUNT, name a few only if it helps the point you are making, "
             "and do not comment on them one at a time or present them as a problem "
             "to fix.\n";
    }

    if (t.noFrames > 0)
    {
        g << "NO FRAMES marks a channel no capture frames arrived from at all. The "
             "reason is UNKNOWN: never say or imply such a channel was silent or was "
             "not outputting signal, because you have no data either way. ";

        // Most of the Links missing is a failed capture. A few is a note. The
        // threshold is a strict majority of the LINKS, since the host is not a
        // Link and can never be in this state.
        const bool mostOfThem = t.links > 0 && t.noFrames * 2 > t.links;
        if (mostOfThem)
        {
            g << t.noFrames << " of " << t.links << " Links are marked NO FRAMES, "
                 "which is most of them, so treat this as a FAILED capture: say so "
                 "plainly and ask them to capture again. Do not review the few "
                 "channels that did arrive as though the session had been measured.\n";
        }
        else
        {
            g << t.noFrames << " of " << t.links << " Links are marked NO FRAMES, "
                 "which is a small part of this capture: mention it once as a note, "
                 "review everything else normally, and do NOT suggest capturing "
                 "again on account of it.\n";
        }
    }

    return g;
}

} // namespace echojay
