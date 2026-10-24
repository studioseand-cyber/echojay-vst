#pragma once
//  EJShadowChannels.h — WHAT THE LIVE COPY'S KEY BUS IS FED (his 12:14 crash, 24 Oct 2026)
//
//  THE ONE SENTENCE: the live copy gets the same shape a rack slot gets - its own buses, with every
//  enabled non-main input bus fed from the main pair - because a declared input bus that is handed
//  nothing is a null pointer the plugin will read.
//
//  WHAT HAPPENED. The third live-view crash, reproduced on the bench and matched frame for frame:
//
//      cl1bST_VST_AU_Protect   +0x1289578   (SIGSEGV, KERN_INVALID_ADDRESS at 0x0)
//      cl1bST_VST_AU_Protect   +0x1290a20
//      cl1bST_VST_AU_Protect   +0xa49f84
//      cl1bST_VST_AU_Protect   +0x128dc14
//      cl1bST_VST_AU_Protect   +0x128d86c
//      cl1bST_VST_AU_Protect   +0x1291d44
//      AudioToolboxCore        AudioUnitRender
//      EchoJay V2              AudioUnitPluginInstanceHeadless::processAudio
//      EchoJay V2              EchoJayProcessor::processBlock
//
//  All six offsets inside the plugin are identical in his Logic crash and in the bench repro, so
//  this is the same defect and not a cousin of it.
//
//  THE CAUSE, in our code. shadowBegin asked the copy for a plain 2-in/2-out and DISABLED every
//  other bus, because the shadow ring carries one stereo pair. The Tube-Tech CL 1B declares two
//  input buses (four channels): main and a key. ChainHost's own note of 4 Oct already wrote down
//  what that costs - *"JUCE's AU host installs a render callback for every DECLARED input bus
//  (prepareToPlay loops `i < getBusCount(isInput)`, no skip for a disabled one), so even a disabled
//  bus has a live callback handing it a 0-channel buffer."*  The plugin pulled its key anyway and
//  read what was behind that empty buffer.
//
//  It is the same family as his 16:33 WaveShell crash (a view narrower than the plugin) and the fix
//  then was the right one for the buffer's WIDTH. This is the other half: width alone is not enough,
//  because a bus can be wide enough and still be fed nothing.
//
//  PROVEN BOTH WAYS ON THE PLUGIN ITSELF, in a supervised bench run Sean authorised (24 Oct):
//      noeditor / product / bigblock / playhead  (the plain-stereo request)  CRASHED in 1-6 s
//      keyfed    (the plugin's own layout, every channel fed)                SURVIVED 60 s, 5620 blocks
//
//  THE RULE IS CHAINHOST'S, NOT A SECOND ONE. ChainHost::rebuildGraph feeds every ENABLED non-main
//  input bus from the slot's own (pre-trimmed) input - self-keyed, at most two channels, a mono key
//  bus getting channel 0 - under Sean's ruling of 6 Oct. The copy must be fed identically or the
//  picture is of a plugin hearing something the rack's plugin never hears, so that loop is written
//  here once, as a plan a test can construct and compare without a plugin or a graph.
#include <JuceHeader.h>
#include <algorithm>
#include <vector>

namespace echojay
{
/** One input bus, as the plan needs to see it - so a guard can describe the Tube-Tech's shape
    without owning a Tube-Tech. `firstChannelInBuffer` is
    AudioProcessor::getChannelIndexInProcessBlockBuffer (true, bus, 0). */
struct ShadowInputBus
{
    int  numChannels          = 0;
    bool enabled              = false;
    int  firstChannelInBuffer = 0;
};

/** One copy the audio thread makes before it renders: buffer channel <- buffer channel. */
struct ShadowChannelCopy
{
    int destChannel = 0;
    int srcChannel  = 0;
};

inline bool operator== (const ShadowChannelCopy& a, const ShadowChannelCopy& b) noexcept
{ return a.destChannel == b.destChannel && a.srcChannel == b.srcChannel; }

/** SELF-KEYING, the same arithmetic ChainHost::rebuildGraph uses for a rack slot:

      - bus 0 is the main input and is already the ring's stereo pair, so it is never in the plan;
      - every later bus that is ENABLED is fed from the main pair, at most two channels;
      - a MONO key bus gets channel 0, the left channel, not an L+R sum (summing needs a mixer and a
        key off one channel of a correlated pair tracks the programme closely enough - ChainHost's
        own reasoning, kept word for word so the two roads cannot drift);
      - a disabled bus is fed nothing, because JUCE gives it a 0-channel buffer and there is no
        channel in our buffer that belongs to it.

    Everything this plan does not name is cleared to silence by the caller: a channel we cannot fill
    must never carry last block's samples. */
inline std::vector<ShadowChannelCopy> selfKeyCopyPlan (const std::vector<ShadowInputBus>& inputBuses)
{
    std::vector<ShadowChannelCopy> plan;

    for (size_t b = 1; b < inputBuses.size(); ++b)
    {
        const auto& bus = inputBuses[b];
        if (! bus.enabled || bus.numChannels <= 0) continue;

        const int fed = std::min (2, bus.numChannels);
        for (int ch = 0; ch < fed; ++ch)
            plan.push_back ({ bus.firstChannelInBuffer + ch, ch });
    }

    return plan;
}

/** The copy's own buses, read once on the message thread. */
inline std::vector<ShadowInputBus> shadowInputBusesOf (const juce::AudioProcessor& p)
{
    std::vector<ShadowInputBus> buses;
    const int n = p.getBusCount (true);
    buses.reserve ((size_t) juce::jmax (0, n));

    for (int b = 0; b < n; ++b)
    {
        const auto* bus = p.getBus (true, b);
        ShadowInputBus s;
        s.numChannels          = bus != nullptr ? bus->getNumberOfChannels() : 0;
        s.enabled              = bus != nullptr && bus->isEnabled();
        s.firstChannelInBuffer = p.getChannelIndexInProcessBlockBuffer (true, b, 0);
        buses.push_back (s);
    }

    return buses;
}

/** The highest buffer channel the plan writes to, so a caller can refuse a plan its buffer cannot
    hold rather than write past the end of it. -1 when the plan is empty. */
inline int highestPlannedChannel (const std::vector<ShadowChannelCopy>& plan) noexcept
{
    int hi = -1;
    for (const auto& c : plan) hi = juce::jmax (hi, c.destChannel, c.srcChannel);
    return hi;
}
} // namespace echojay
