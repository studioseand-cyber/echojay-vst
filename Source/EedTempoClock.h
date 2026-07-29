/*
    EedTempoClock.h  —  the host tempo, published once and readable by any
    built-in device that syncs to it.

    WHY THIS EXISTS. A tempo-synced delay needs the DAW's BPM, and a built-in
    device cannot reach it. Built-ins run inside ChainHost's AudioProcessorGraph,
    and nothing sets a playhead on that graph — the graph forwards ITS playhead to
    its nodes, but its own is never assigned, so every hosted processor (ours and
    third-party) sees getPlayHead() == nullptr. Fixing that properly means giving
    the graph the host's playhead, which changes what every hosted third-party
    plugin sees mid-session, and is not this cluster's call to make.

    So instead: EchoJayProcessor already reads the playhead at the top of every
    block for its own transport tracking. One line there publishes the BPM here,
    and any device that wants it reads it lock-free. That is the entire coupling —
    one write site, no plumbing through ChainHost, and a device that is auditioned
    somewhere with no host tempo at all still works, on the fallback.

    THE FALLBACK IS NOT AN ERROR PATH. Standalone, offline render, a host that
    reports no tempo, EchoJay Link (which shares ChainHost but has its own
    processor and does not publish) — all of these are normal, and all of them get
    120 BPM. A synced delay then still delays; it just delays at 120 rather than
    at nothing, which is the difference between a device that degrades and a
    device that breaks.

    JUCE-free so the engines that read it stay g++-testable.
*/

#pragma once

#include <atomic>

namespace echojay
{

// The tempo any synced device reads. Written from the audio thread (the host
// gives us the playhead there), read from the audio thread. A plain relaxed
// atomic double: it is one independent value with no ordering relationship to
// anything else, and a reader that catches the previous block's tempo is
// indistinguishable from one that caught this block's.
inline std::atomic<double> gHostTempoBpm { 120.0 };

// Sane musical bounds. A host that reports 0 (stopped, or simply not
// implemented) must not turn into a division by zero that produces an infinite
// delay time — which is silence, and looks exactly like a broken device.
inline constexpr double kMinTempoBpm = 20.0;
inline constexpr double kMaxTempoBpm = 999.0;
inline constexpr double kDefaultTempoBpm = 120.0;

// Call from wherever the host playhead is already being read.
inline void publishHostTempo (double bpm) noexcept
{
    if (! (bpm >= kMinTempoBpm && bpm <= kMaxTempoBpm)) return;   // also rejects NaN
    gHostTempoBpm.store (bpm, std::memory_order_relaxed);
}

// Always a usable number.
inline double hostTempoBpm() noexcept
{
    const double b = gHostTempoBpm.load (std::memory_order_relaxed);
    return (b >= kMinTempoBpm && b <= kMaxTempoBpm) ? b : kDefaultTempoBpm;
}

} // namespace echojay
