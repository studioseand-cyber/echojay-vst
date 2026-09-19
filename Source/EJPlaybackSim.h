#pragma once

#include <JuceHeader.h>
#include "EJPlaybackVoicing.h"   // echojay::VoicingChain: the eight device voicings

// =============================================================================
//  PLAYBACK SIMULATION: the inline stage, and where it is allowed to live.
// =============================================================================
//
// This is the stage, placed and argued, so that the placement was decided once
// by someone holding the whole argument rather than by whoever added the first
// curve.
//
// THE SELECTION. None, the mono fold, and eight device voicings. Each voicing is
// a device class (a phone speaker, a laptop, a car), never a brand: decision 11
// of COMPARE_REFERENCE_PLAN, because a generic response curve cannot deliver the
// fidelity a manufacturer's name implies. The voicings' numbers live in
// EJPlaybackVoicing.h; here they are only mapped and run.
//
// NO CARD SELECTS A VOICING YET. The five values exist, have bodies and are
// pinned, but nothing in the interface stores them, so outside a test driving
// the stage they are not audible.
enum class PlaybackSim
{
    None = 0,     ///< no simulation; the stage is a no-op and must stay one
    MonoFold,     ///< sum to mono at unity for a centred source
    PhoneSpeaker, ///< echojay::PlaybackVoicing::PhoneSpeaker, through the chain
    Laptop,       ///< echojay::PlaybackVoicing::Laptop
    CarDashboard, ///< echojay::PlaybackVoicing::CarDashboard
    KitchenRadio, ///< echojay::PlaybackVoicing::KitchenRadio
    Earbuds,      ///< echojay::PlaybackVoicing::Earbuds
    TvSoundbar,       ///< echojay::PlaybackVoicing::TvSoundbar, stereo
    BluetoothSpeaker, ///< echojay::PlaybackVoicing::BluetoothSpeaker, mono first
    ClubPA,           ///< echojay::PlaybackVoicing::ClubPA, mono first

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

/** The selection's name, for FAIL lines and diagnostics. No default label, so
    -Wswitch-enum names any value added to the enum without a name here. */
inline const char* playbackSimName (PlaybackSim s) noexcept
{
    switch (s)
    {
        case PlaybackSim::None:         return "None";
        case PlaybackSim::MonoFold:     return "MonoFold";
        case PlaybackSim::PhoneSpeaker: return "PhoneSpeaker";
        case PlaybackSim::Laptop:       return "Laptop";
        case PlaybackSim::CarDashboard: return "CarDashboard";
        case PlaybackSim::KitchenRadio: return "KitchenRadio";
        case PlaybackSim::Earbuds:      return "Earbuds";
        case PlaybackSim::TvSoundbar:       return "TvSoundbar";
        case PlaybackSim::BluetoothSpeaker: return "BluetoothSpeaker";
        case PlaybackSim::ClubPA:           return "ClubPA";
        case PlaybackSim::Count:        return "Count";
    }
    return "(out of range)";
}

/** THE STORED SELECTION, WITH ITS REFUSAL RULE, and both live here rather than
    in the processor so the suite can exercise the shipped code instead of a
    copy of it. mapfps_test cannot link a processor, and a pin written against a
    reimplementation of this rule would pass while the real one rotted.

    WHY A REFUSAL AT ALL. Count is not a selection, it is the sweep's bound.
    Storing it produces a state where playbackSimActive returns true, because
    Count is not None, and applyPlaybackSim then falls past the switch and runs
    nothing. That is a card the user can select and hear nothing from, with no
    error and nothing on screen. pb PIN7 catches that shape in the suite; this
    catches the same shape at runtime, where no test can reach.

    A REFUSED VALUE LEAVES THE STORED SELECTION UNCHANGED. The selection the
    user last made keeps playing rather than silently reverting to None: a
    refusal is a rejected instruction, not a request to stop.

    RELAXED ON BOTH SIDES, stated once, here. The audio thread reads this every
    block and the editor writes it; a block either side of the change is equally
    correct and nothing else is ordered against this write.

    Pinned by pb PIN8. */
class PlaybackSimSelection
{
public:
    void set (PlaybackSim s) noexcept
    {
        const int v = (int) s;
        if (v <  (int) PlaybackSim::None)  return;   // a cast from below zero
        if (v >= (int) PlaybackSim::Count) return;   // Count, and past it
        value_.store (s, std::memory_order_relaxed);
    }

    PlaybackSim get() const noexcept
    {
        return value_.load (std::memory_order_relaxed);
    }

private:
    std::atomic<PlaybackSim> value_ { PlaybackSim::None };
};

class PlaybackSimStage;
inline bool applyPlaybackSim (PlaybackSimStage& stage, float* const* channels,
                              int numChannels, int numSamples) noexcept;

/** THE FOLD: both channels become (L + R) * 0.5, in place. ONE DEFINITION,
    used by the Mono tile and by every voicing whose row sums to mono first,
    so the two cannot come to disagree about what "mono" means.

    The caller has already refused a missing or aliased pair; this only folds.

    ONE LOCAL, WRITTEN TWICE. Writing the expression into each channel
    separately would be two expressions that agree today and could stop
    agreeing after any edit to either line. Computed once, the two channels are
    identical BY CONSTRUCTION rather than by coincidence, which is what pb PIN4
    asserts.

    THE GAIN IS 0.5f, NOT 0.70710678f. A centred source must come out at exactly
    the level it went in. With the power-preserving gain it would come out 3 dB
    up, and then every environment on this page reads as "mono is louder", and
    the collapse the user is actually listening for gets buried under a level
    change they did not ask for. The point of the fold is to hear what survives
    the collapse, not to hear a different volume.

    AND BOTH CLAIMS ARE EXACT, NOT APPROXIMATE. In binary floating point L + L
    only increments the exponent and multiplying by 0.5 only decrements it, with
    the significand untouched, so a centred source comes out BIT-IDENTICAL rather
    than nearly unchanged. L + (-L) is exactly +0.0 under round-to-nearest, so
    anti-correlated content vanishes completely rather than nearly. pb PIN2 and
    pb PIN3 assert both with memcmp rather than a tolerance, because a tolerance
    would also pass an implementation that is merely close. */
inline void monoFoldInPlace (float* left, float* right, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        float m = left[i] + right[i];
        m *= 0.5f;
        left[i]  = m;
        right[i] = m;
    }
}

/** THE STAGE: the selection, the filters a voicing needs, and one block of
    memory about which voicing ran last.

    applyPlaybackSim used to be a free function of the selection alone, with no
    state and no sample rate. A voicing needs both, so the stage owns them and
    applyPlaybackSim takes the stage. There is ONE apply path; the old
    signature is gone rather than kept beside this one.

    THE SELECTION is a PlaybackSimSelection, unchanged: the same atomic, the
    same relaxed ordering and the same refusal rule, pinned by pb PIN8.

    THE CHAINS: one echojay::VoicingChain per channel, for up to two channels,
    audio thread only apart from prepare(). A mono buffer uses the first.

    THE PREVIOUS VOICING is what the last block ran through the chains.
    None, and the mono fold, both count as "no voicing": neither touches them. */
class PlaybackSimStage
{
public:
    /** Called from prepareToPlay, which has the rate (PluginProcessor.cpp:564).

        THE STATE IS ZEROED HERE, UNCONDITIONALLY, on every prepare. That is the
        rule EqEngine::prepare and MeterEngine::prepare follow for filter
        memory. The other rule in this codebase, clearing only when the rate
        actually changes (ChainHost.cpp:849), protects the level tallies'
        accumulated measurements, which a host's routine re-prepare would
        otherwise wipe. A few samples of filter memory are not a measurement,
        and after a rate change they are not even valid.

        The previous voicing becomes None, so the next voiced block is an
        activation and sets its coefficients at the new rate. */
    void prepare (double sampleRate) noexcept
    {
        for (auto& c : chains_)
            c.prepare (sampleRate);
        previousVoicing_ = echojay::PlaybackVoicing::None;
    }

    /** Stores a selection, or refuses it: PlaybackSimSelection::set's rule. */
    void select (PlaybackSim s) noexcept { selection_.set (s); }

    PlaybackSim selected() const noexcept { return selection_.get(); }

private:
    friend bool applyPlaybackSim (PlaybackSimStage&, float* const*, int, int) noexcept;

    /** Runs voicing v through the chains, in place. Returns true when it ran,
        under applyPlaybackSim's contract. */
    bool runVoicing (echojay::PlaybackVoicing v, float* const* channels,
                     int numChannels, int numSamples) noexcept
    {
        // THE GUARDS. Each returns false having written nothing, because under
        // the contract false means the body did not run. The previous voicing
        // is left alone too: nothing ran, so nothing about the chains changed.
        if (numChannels < 1)     return false;
        if (channels == nullptr) return false;
        float* const left = channels[0];
        if (left == nullptr)     return false;

        // A MONO BUFFER, OR THE ALIASED PAIR THE CALL SITE BUILDS FOR ONE, IS
        // FILTERED ONCE. Two chains over one buffer would filter it twice,
        // which is a different response and not the voicing.
        float* const right = numChannels >= 2 ? channels[1] : nullptr;
        const bool stereo = (right != nullptr && right != left);

        // THE RESET RULE, AND IT IS THE ONE DECISION IN THIS STAGE.
        //
        // FROM NONE TO A VOICING, THE CHAINS ARE ZEROED. Their memory is
        // whatever they last saw before the voicing was turned off, which may
        // be seconds or minutes ago and may be full scale. Keeping it would
        // inject two samples per section from an arbitrary earlier moment into
        // the first samples of audio that has nothing to do with them: a
        // transient nobody played.
        //
        // FROM ONE LIVE VOICING TO ANOTHER, THE CHAINS ARE NOT ZEROED. That
        // would empty a filter in the middle of a signal, which is the click
        // EedDynamicsCore.h:225 exists to avoid ("a state reset is audible").
        // So setVoicing is called, which calls setCoeffs and keeps the state
        // deliberately, and the new voicing continues from where the old one
        // left the signal.
        //
        // The same voicing as last block needs neither. Pinned by pb PIN10
        // (activation zeroes) and pb PIN11 (a switch does not).
        if (previousVoicing_ == echojay::PlaybackVoicing::None)
        {
            for (auto& c : chains_)
            {
                c.setVoicing (v);
                c.reset();                              // THE ACTIVATION RESET
            }
        }
        else if (previousVoicing_ != v)
        {
            for (auto& c : chains_)
                c.setVoicing (v);                       // state kept, on purpose
        }
        previousVoicing_ = v;

        // MONO FIRST, FILTERS AFTER, for a voicing whose row says so (a
        // portable speaker, a PA): one source, so the pair is folded and THEN
        // voiced, the order the device plays it in. With identical linear
        // filters on both channels the other order would give the same samples
        // up to rounding; what matters is the fold, which collapses the image
        // and cancels anti-phase content (VoicingRow::monoFirst). A mono buffer
        // is one channel already and is not folded against itself.
        //
        // BOTH CHAINS RUN, on what is now the same signal, so each chain's
        // memory stays current for the next voicing. From an activation both
        // chains start zeroed, so the two channels are bit-identical from the
        // first sample (pb PIN12). After a switch from a stereo voicing the two
        // chains keep their own few samples of memory, so the channels differ
        // until that decays, a few milliseconds; the reset rule above keeps
        // state across a switch on purpose, and this does not override it.
        if (stereo && echojay::voicingSumsToMono (v))
            monoFoldInPlace (left, right, numSamples);

        chains_[0].process (left, numSamples);
        if (stereo)
            chains_[1].process (right, numSamples);

        // Zero samples still returns true, for the reason the mono fold gives:
        // the body ran, over nothing.
        return true;
    }

    PlaybackSimSelection     selection_;
    echojay::VoicingChain    chains_[2];
    echojay::PlaybackVoicing previousVoicing_ = echojay::PlaybackVoicing::None;
};

/** The stage itself: samples in place, or nothing at all.

    REAL-TIME SAFE, and it has to stay that way. No allocation, no locks, no
    file access, no logging. The voicings' filter state lives in the stage: it
    is zeroed in prepareToPlay (PlaybackSimStage::prepare) and otherwise touched
    only here, on the audio thread. The selection is read ONCE per call, so a
    block runs one selection from its first sample to its last.

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
inline bool applyPlaybackSim (PlaybackSimStage& stage, float* const* channels,
                              int numChannels, int numSamples) noexcept
{
    const PlaybackSim s = stage.selection_.get();

    // THE EARLY-OUT. It writes one thing, and never to the buffer: the record
    // that this block ran no voicing, so the next voiced block is an
    // activation and starts from zeroed filters (pb PIN10).
    if (! playbackSimActive (s))
    {
        stage.previousVoicing_ = echojay::PlaybackVoicing::None;
        return false;
    }

    switch (s)
    {
        case PlaybackSim::MonoFold:
        {
            // The fold runs no voicing, so for the reset rule this block is
            // "none", exactly as if nothing were selected. Everything below
            // this line is the fold as it was: unfiltered, and bit-identical.
            stage.previousVoicing_ = echojay::PlaybackVoicing::None;

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

            // THE FOLD ITSELF, now one function shared with the voicings that
            // sum to mono first. The loop, the 0.5f and why both are exact are
            // at monoFoldInPlace, unchanged; this case is the Mono tile exactly
            // as it was.
            monoFoldInPlace (left, right, numSamples);

            // ZERO SAMPLES RETURNS TRUE, DECIDED RATHER THAN EMERGENT. A block
            // of zero samples is a loop with zero iterations: the stage ran, it
            // simply had nothing to run over. Returning false there would make
            // the return a statement about the block's length rather than about
            // whether the simulation executed, and the caller cannot tell those
            // apart afterwards.
            return true;
        }

        // THE EIGHT VOICINGS. One case each, mapping the selection to its row
        // and running the chain, so deleting any one of them sends that value
        // to the trailing false below, where pb PIN7 names it.
        case PlaybackSim::PhoneSpeaker:
            return stage.runVoicing (echojay::PlaybackVoicing::PhoneSpeaker,
                                     channels, numChannels, numSamples);
        case PlaybackSim::Laptop:
            return stage.runVoicing (echojay::PlaybackVoicing::Laptop,
                                     channels, numChannels, numSamples);
        case PlaybackSim::CarDashboard:
            return stage.runVoicing (echojay::PlaybackVoicing::CarDashboard,
                                     channels, numChannels, numSamples);
        case PlaybackSim::KitchenRadio:
            return stage.runVoicing (echojay::PlaybackVoicing::KitchenRadio,
                                     channels, numChannels, numSamples);
        case PlaybackSim::Earbuds:
            return stage.runVoicing (echojay::PlaybackVoicing::Earbuds,
                                     channels, numChannels, numSamples);
        case PlaybackSim::TvSoundbar:
            return stage.runVoicing (echojay::PlaybackVoicing::TvSoundbar,
                                     channels, numChannels, numSamples);
        case PlaybackSim::BluetoothSpeaker:
            return stage.runVoicing (echojay::PlaybackVoicing::BluetoothSpeaker,
                                     channels, numChannels, numSamples);
        case PlaybackSim::ClubPA:
            return stage.runVoicing (echojay::PlaybackVoicing::ClubPA,
                                     channels, numChannels, numSamples);

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
