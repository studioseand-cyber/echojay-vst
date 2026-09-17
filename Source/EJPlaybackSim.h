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
    MonoFold,     ///< sum to mono at unity for a centred source

    /** SENTINEL, ALWAYS LAST. NEW VALUES GO ABOVE THIS LINE, NEVER BELOW IT.

        pb PIN7 sweeps every value from the one after None up to Count and
        requires each of them to report that its body ran. A value added below
        Count would be outside the sweep and could ship with no body at all,
        which is the one failure this arrangement exists to prevent. */
    Count
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

    switch (s)
    {
        case PlaybackSim::MonoFold:
        {
            // THE GUARDS. Each returns false having written nothing, because
            // under the contract above false means the body did not run.
            if (numChannels < 2)     return false;
            if (channels == nullptr) return false;

            float* const left  = channels[0];
            float* const right = channels[1];
            if (left == nullptr || right == nullptr) return false;

            // THE ALIASED PAIR IS ROUTINE, NOT PARANOIA. isBusesLayoutSupported
            // accepts mono, and on a one-channel buffer the call site fills
            // chans[1] with getWritePointer(0), so both pointers name the same
            // memory as a matter of course. A fold that read right[i] without
            // this check would read back what it had just written to left[i],
            // and the second sample onward would be folded against itself.
            if (left == right) return false;

            // ONE LOCAL, WRITTEN TWICE. Writing the expression into each
            // channel separately would be two expressions that agree today and
            // could stop agreeing after any edit to either line. Computed once,
            // the two channels are identical BY CONSTRUCTION rather than by
            // coincidence, which is what pb PIN4 asserts.
            //
            // THE GAIN IS 0.5f, NOT 0.70710678f. A centred source must come out
            // at exactly the level it went in. With the power-preserving gain it
            // would come out 3 dB up, and then every environment on this page
            // reads as "mono is louder", and the collapse the user is actually
            // listening for gets buried under a level change they did not ask
            // for. The point of the fold is to hear what survives the collapse,
            // not to hear a different volume.
            //
            // AND BOTH CLAIMS ARE EXACT, NOT APPROXIMATE. In binary floating
            // point L + L only increments the exponent and multiplying by 0.5
            // only decrements it, with the significand untouched, so a centred
            // source comes out BIT-IDENTICAL rather than nearly unchanged.
            // L + (-L) is exactly +0.0 under round-to-nearest, so anti-
            // correlated content vanishes completely rather than nearly. pb PIN2
            // and pb PIN3 assert both with memcmp rather than a tolerance,
            // because a tolerance would also pass an implementation that is
            // merely close.
            for (int i = 0; i < numSamples; ++i)
            {
                float m = left[i] + right[i];
                m *= 0.5f;
                left[i]  = m;
                right[i] = m;
            }

            // ZERO SAMPLES RETURNS TRUE, DECIDED RATHER THAN EMERGENT. A block
            // of zero samples is a loop with zero iterations: the stage ran, it
            // simply had nothing to run over. Returning false there would make
            // the return a statement about the block's length rather than about
            // whether the simulation executed, and the caller cannot tell those
            // apart afterwards.
            return true;
        }

        case PlaybackSim::None:
        case PlaybackSim::Count:
            // None never reaches here (the early-out took it) and Count is a
            // sentinel, not a selection. Both are listed so -Wswitch-enum stays
            // quiet WITHOUT a default label: a default would swallow a newly
            // added value silently, and the whole arrangement depends on a new
            // value being visible rather than absorbed.
            break;
    }

    // REACHED ONLY BY A VALUE WITH NO CASE ABOVE, which is the failure pb PIN7
    // is built to catch. An active selection that arrives here has run nothing,
    // so false is the honest answer: the body did not execute because there is
    // no body. A new environment added without a case falls through to exactly
    // this line, and the sweep says which value did it.
    return false;
}
