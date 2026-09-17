#pragma once

#include <JuceHeader.h>

// =============================================================================
//  PLAYBACK SIMULATION: the inline stage, and where it is allowed to live.
// =============================================================================
//
// Nothing simulates anything yet. This is the stage, empty, placed and argued,
// so that the placement is decided once by someone holding the whole argument
// rather than by whoever adds the first curve.
//
// THE SELECTION. None is the only value today. A later value is a device class
// (a phone speaker, a laptop, a car), never a brand: decision 11 of
// COMPARE_REFERENCE_PLAN, because a generic response curve cannot deliver the
// fidelity a manufacturer's name implies.
enum class PlaybackSim
{
    None = 0,     ///< no simulation; the stage is a no-op and must stay one
};

/** Does this selection change the audio at all?

    THE EARLY-OUT IS THE CONTRACT, not an optimisation. The stage runs on the
    audio thread on every block for every user, and the overwhelming majority of
    them will never select a simulation. A stage that touches the buffer when
    nothing is selected is a stage that can introduce a defect into audio nobody
    asked it to touch, and it would do so silently because the output is
    supposed to be unchanged.

    Pinned by ps PIN1, and the mutation that removes the early-out is the exact
    defect it exists to prevent. */
inline bool playbackSimActive (PlaybackSim s) noexcept
{
    return s != PlaybackSim::None;
}

/** The stage itself: samples in place, or nothing at all.

    REAL-TIME SAFE, and it has to stay that way. No allocation, no locks, no
    file access, no logging. When a curve arrives it brings filter state with
    it, and that state is prepared in prepareToPlay and only ever read here.

    Returns true when it touched the buffer, so a caller can assert the no-op
    rather than trust it. */
inline bool applyPlaybackSim (PlaybackSim s, float* const* channels,
                              int numChannels, int numSamples) noexcept
{
    if (! playbackSimActive (s)) return false;          // THE EARLY-OUT
    juce::ignoreUnused (channels, numChannels, numSamples);

    // No simulation exists yet. When one does it goes here, and it must leave
    // the early-out above intact.
    return false;
}
