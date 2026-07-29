/*
    EedModTempo.h  —  host tempo into the shared LFO, for the Modulation cluster.

    LfoCore is JUCE-free by design, so it cannot read a playhead; it takes a BPM.
    This is the four-line bridge, written once because getting it wrong is silent:
    a device that skips it simply never syncs, and the SYNC button looks like it
    works because the DIVISION dial still turns.

    Call it at the TOP of processBlock, before any audio is touched. The playhead
    is only valid inside a callback — the graph installs it on each node as it
    processes (AudioProcessorGraph hands every node the host's playhead), so
    asking outside a callback gets whatever was left over.

    Absent tempo is normal, not exceptional: an offline render, a host that does
    not publish one, or the device auditioned standalone in auval. LfoCore keeps
    the last good value (or its 120 BPM stand-in), so a synced device keeps
    modulating instead of freezing at a rate of zero.
*/

#pragma once

#include <JuceHeader.h>
#include "EedLfoCore.h"

namespace echojay
{

inline void pushHostTempo (juce::AudioProcessor& proc, LfoCore& lfo)
{
    if (auto* ph = proc.getPlayHead())
        if (const auto pos = ph->getPosition())
            if (const auto bpm = pos->getBpm())
                lfo.setTempoBpm (*bpm);
}

} // namespace echojay
