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

    Pinned by pb PIN1, and the mutation that removes the early-out is the exact
    defect it exists to prevent. (pb, not ps: ps is the PSR floor family's
    prefix, and two subjects under one pin name means a FAIL line cannot say
    which of them broke.) */
inline bool playbackSimActive (PlaybackSim s) noexcept
{
    return s != PlaybackSim::None;
}

/** The stage itself: samples in place, or nothing at all.

    REAL-TIME SAFE, and it has to stay that way. No allocation, no locks, no
    file access, no logging. When a curve arrives it brings filter state with
    it, and that state is prepared in prepareToPlay and only ever read here.

    THE RETURN VALUE MEANS THE SIMULATION BODY RAN. True: the selection's body
    executed. False: it did not.

    IT IS NOT A CLAIM THAT ANY SAMPLE CHANGED, and reading it as one will
    mislead you. A mono fold applied to channels that already hold identical
    audio runs in full and leaves every sample bit-identical. It returns true,
    because it ran; the buffer is untouched, because there was nothing to
    change. Tying the return to sample values would make it a function of
    CONTENT rather than of selection, and then no fixed test signal could
    establish whether the stage executed.

    IT IS FALSE WHEN THE STAGE CANNOT RUN AT ALL: nothing is selected, or the
    buffer has too few channels for the selected simulation to be defined.

    THE NO-OP IS NOT PINNED BY THIS VALUE. pb PIN1 copies the buffer and
    compares it with memcmp afterwards, because the memcmp READS THE BUFFER
    while this return only comments on it. A return value that lied would pass
    a test built on the return value. */
inline bool applyPlaybackSim (PlaybackSim s, float* const* channels,
                              int numChannels, int numSamples) noexcept
{
    if (! playbackSimActive (s)) return false;          // THE EARLY-OUT
    juce::ignoreUnused (channels, numChannels, numSamples);

    // No simulation exists yet. When one does it goes here, and it must leave
    // the early-out above intact.
    //
    // THIS false IS A PLACEHOLDER AND IS NOT THE HOUSE STYLE. Reaching this
    // line means the selection was ACTIVE, so under the contract above the
    // honest answer is already true: an active path that got here has run. It
    // stays false only because there is no body yet to have run, and inventing
    // a branch to dress that up would be worse than one wrong word.
    //
    // THE FIRST REAL SIMULATION MUST RETURN true HERE, on every path where its
    // body executed, INCLUDING the paths where the audio came out identical.
    // A fold of two identical channels ran. If it returns false because nothing
    // audibly changed, the return silently becomes a claim about content, the
    // contract above is void, and no test can tell "the stage did not run" from
    // "the stage ran on symmetrical audio".
    //
    // It returns false ONLY when it genuinely could not run: too few channels
    // for the selected simulation, or state that prepareToPlay has not made
    // ready yet.
    return false;
}
