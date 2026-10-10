#pragma once

#include <JuceHeader.h>
#include "EJPlaybackVoicing.h"   // echojay::VoicingChain: the eight device voicings
#include "EJPlaybackRoom.h"      // echojay::PlaybackRoom and its table; the reverb engine through it

#include <algorithm>
#include <cmath>
#if defined(__APPLE__)
 #include <mach/mach_time.h>     // the idle-gap rule's clock: mach_absolute_time
#else
 #include <chrono>
#endif

// =============================================================================
//  PLAYBACK SIMULATION: the inline stage, and where it is allowed to live.
// =============================================================================
//
// This is the stage, placed and argued, so that the placement was decided once
// by someone holding the whole argument rather than by whoever added the first
// curve.
//
// THE SELECTION. None, the mono fold, eight device voicings and four rooms. A
// ninth voicing, FestivalPA, has no selection of its own: it is the festival
// field's source, the club system heard across a field, and it is reached only
// through that room. Each voicing is a device class (a phone speaker, a laptop,
// a car), never a brand:
// decision 11 of COMPARE_REFERENCE_PLAN, because a generic response curve cannot
// deliver the fidelity a manufacturer's name implies. The voicings' numbers live
// in EJPlaybackVoicing.h and the rooms' in EJPlaybackRoom.h; here they are only
// mapped and run.
//
// EVERY SELECTION HAS A TILE on the Playback page's grid (EJPlaybackTiles.h),
// which is what stores it; pb PIN9 holds the two together. (This paragraph used
// to say no card selected a voicing, which stopped being true when the grid
// arrived.)
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
    Bedroom,          ///< echojay::PlaybackRoom::Bedroom: the reverb, source None
    SmallBar,         ///< echojay::PlaybackRoom::SmallBar, source None
    ClubFloor,        ///< echojay::PlaybackRoom::ClubFloor, source ClubPA: the PA in its room
    FestivalField,    ///< echojay::PlaybackRoom::FestivalField, source FestivalPA: the PA at a distance

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
        case PlaybackSim::Bedroom:          return "Bedroom";
        case PlaybackSim::SmallBar:         return "SmallBar";
        case PlaybackSim::ClubFloor:        return "ClubFloor";
        case PlaybackSim::FestivalField:    return "FestivalField";
        case PlaybackSim::Count:        return "Count";
    }
    return "(out of range)";
}

/** The room a selection plays into, or None for every selection that is not a
    room. No default label, so -Wswitch-enum makes each new selection say
    whether it is one. pr PIN1 checks that every room belongs to exactly one
    selection. */
inline echojay::PlaybackRoom playbackSimRoom (PlaybackSim s) noexcept
{
    switch (s)
    {
        case PlaybackSim::Bedroom:          return echojay::PlaybackRoom::Bedroom;
        case PlaybackSim::SmallBar:         return echojay::PlaybackRoom::SmallBar;
        case PlaybackSim::ClubFloor:        return echojay::PlaybackRoom::ClubFloor;
        case PlaybackSim::FestivalField:    return echojay::PlaybackRoom::FestivalField;

        case PlaybackSim::None:
        case PlaybackSim::MonoFold:
        case PlaybackSim::PhoneSpeaker:
        case PlaybackSim::Laptop:
        case PlaybackSim::CarDashboard:
        case PlaybackSim::KitchenRadio:
        case PlaybackSim::Earbuds:
        case PlaybackSim::TvSoundbar:
        case PlaybackSim::BluetoothSpeaker:
        case PlaybackSim::ClubPA:
        case PlaybackSim::Count:
            break;
    }
    return echojay::PlaybackRoom::None;
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
                              int numChannels, int numSamples, double nowSeconds) noexcept;
inline bool playbackSimBody (PlaybackSimStage& stage, PlaybackSim s, float* const* channels,
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
    None, the mono fold and a room with no source all count as "no voicing":
    none of them touches the chains.

    THE ROOM (19 Sep 2026) is a reverb network, echojay::ReverbEngine, with
    STATE OF ITS OWN, roomHeld_, and a rule of its own. It does not use the
    previous voicing: that says "was any voicing running", and a room, then the
    phone speaker for ten minutes, then the room again would find it set to the
    phone, reset nothing, and replay a tail frozen ten minutes ago (open list
    194). The two rules are independent, so a room that has a source device runs
    that device's chains under the voicing rule and its network under this one. */
class PlaybackSimStage
{
public:
    /** Called from prepareToPlay, which has the rate (PluginProcessor.cpp:569).

        THE STATE IS ZEROED HERE, UNCONDITIONALLY, on every prepare. That is the
        rule EqEngine::prepare and MeterEngine::prepare follow for filter
        memory. The other rule in this codebase, clearing only when the rate
        actually changes (ChainHost.cpp:849), protects the level tallies'
        accumulated measurements, which a host's routine re-prepare would
        otherwise wipe. A few samples of filter memory are not a measurement,
        and after a rate change they are not even valid.

        The previous voicing becomes None, so the next voiced block is an
        activation and sets its coefficients at the new rate. No room is held,
        so the next room block is an engagement.

        IT ALLOCATES, SO IT IS NOT noexcept (19 Sep 2026). ReverbEngine::prepare
        sizes the room's delay lines: 320 KB at 44.1 and 48 kHz, 640 KB at 96,
        1.28 MB at 192. An allocation failure inside a noexcept function
        terminates the process. The one caller, prepareToPlay, is not noexcept
        and already allocates three lines earlier (cmpTmpBuf, cmpMixBuf,
        cmpGainScratch), so a failure here now behaves like one of those. */
    void prepare (double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        for (auto& c : chains_)
            c.prepare (sampleRate_);
        previousVoicing_ = echojay::PlaybackVoicing::None;

        // THE ROOM'S SETTINGS GO IN BEFORE THE ENGINE'S prepare, which derives
        // the line lengths from them and snaps the lines there. See the
        // engagement reset in runRoom for why the order matters (open list 195).
        applyRoomSettings (echojay::PlaybackRoom::Bedroom);
        room_.prepare (sampleRate_, kRoomChunkSamples);

        roomHeld_         = echojay::PlaybackRoom::None;
        roomPos_          = 0;
        roomFadeLen_      = std::max (1, (int) std::lround (kRoomFadeSeconds * sampleRate_));
        lastRoomSeconds_  = -1.0;
        lastRoomBlockSec_ = 0.0;

       #if defined(__APPLE__)
        // The timebase, read here on the message thread so the audio thread
        // never has to: 1/1 on Intel, 125/3 on Apple silicon.
        mach_timebase_info_data_t tb {};
        mach_timebase_info (&tb);
        ticksToSeconds_ = (tb.denom != 0) ? (double) tb.numer / (double) tb.denom * 1.0e-9 : 1.0e-9;
       #endif
    }

    /** Stores a selection, or refuses it: PlaybackSimSelection::set's rule. */
    void select (PlaybackSim s) noexcept { selection_.set (s); }

    PlaybackSim selected() const noexcept { return selection_.get(); }

    /** THE CLOCK THE IDLE-GAP RULE READS, in seconds. mach_absolute_time on
        Apple, which is a read of a counter and safe on the audio thread. The
        processor passes this to applyPlaybackSim on every block; the suite
        passes times of its own, which is why it is a parameter there rather
        than a read inside the stage. */
    double clockSeconds() const noexcept
    {
       #if defined(__APPLE__)
        return (double) mach_absolute_time() * ticksToSeconds_;
       #else
        return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
       #endif
    }

    /** The room's fade, in samples at the prepared rate: 30 ms. For pr PIN2. */
    int roomFadeSamples() const noexcept { return roomFadeLen_; }

    /** The room's processing chunk: the dry copy the fade needs is this long,
        so a block of any length is run through the room in pieces of at most
        this many samples. A fixed size, so the stage needs no block size from
        the host and allocates nothing for the fade. */
    static constexpr int kRoomChunkSamples = 256;

private:
    friend bool applyPlaybackSim (PlaybackSimStage&, float* const*, int, int, double) noexcept;
    friend bool playbackSimBody  (PlaybackSimStage&, PlaybackSim, float* const*, int, int) noexcept;

    /** 30 ms, LINEAR, IN AND OUT. The project already ruled on an instant step
        in a path someone listens through: editSoloMix_ got a real 30 ms ramp
        because its engage and release were "an instant step, i.e. a click"
        (PluginProcessor.cpp, prepareToPlay). A reverb switched off mid-tail
        is the same step with a longer tail behind it.

        NOT THE ENGINE'S OWN MIX SMOOTHER. That is exponential with a 20 ms
        time constant (EedReverbEngine.h, mixSmooth_), so it takes about 140 ms
        to fall 60 dB and never reaches zero, which would keep "off" audibly
        reverberating for longer and still end on a step. */
    static constexpr double kRoomFadeSeconds = 0.030;

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

    /** A room selection's BODY: its source device through the chains, under
        the voicing rule above, or nothing for a room with no source. The
        room's network runs after the body, in runRoom, for every selection. */
    bool runRoomSource (echojay::PlaybackRoom r, float* const* channels,
                        int numChannels, int numSamples) noexcept
    {
        const auto source = echojay::roomRow (r).source;
        if (source != echojay::PlaybackVoicing::None)
            return runVoicing (source, channels, numChannels, numSamples);

        // THE ROOM ALONE: the mix as it is, played into the space. The same
        // guards as runVoicing, and, like the mono fold, for the voicing rule
        // this block ran no voicing, so a device selected next is an activation.
        if (numChannels < 1 || channels == nullptr || channels[0] == nullptr)
            return false;
        previousVoicing_ = echojay::PlaybackVoicing::None;
        return true;
    }

    /** Room r's settings, from its row, into the engine. Mod depth, duck and
        width are never set: the engine's defaults stand (EJPlaybackRoom.h,
        RoomRow, says why). */
    void applyRoomSettings (echojay::PlaybackRoom r) noexcept
    {
        const auto& row = echojay::roomRow (r);
        room_.setAlgorithm    (row.algorithm);
        room_.setSizePct      (row.sizePct);
        room_.setDecaySeconds (row.decaySec);
        room_.setPredelayMs   (row.predelayMs);
        room_.setDampingPct   (row.dampingPct);
        room_.setLowCutHz     (row.lowCutHz);
        room_.setEarlyLatePct (row.earlyLatePct);
        room_.setDiffusionPct (row.diffusionPct);
        room_.setMixPct       (row.mixPct);
    }

    /** How long the held room's tail lasts: THE EchoJay Reverb PLUGIN'S OWN
        FORMULA, EedReverbProcessor::getTailLengthSeconds, the predelay plus 1.2
        times the decay the damping leaves. Written out again because that is a
        member of another processor; pr PIN7 holds the two texts alike, so an
        edit to one without the other reddens. */
    double roomTailSeconds() const noexcept
    {
        const double pre = (double) room_.getPredelayMs() * 0.001;
        return pre + room_.effectiveDecaySeconds() * 1.2;
    }

    /** THE ROOM, run after the selection's body on every block. Returns true
        when it wrote the buffer.

        THE ENGAGEMENT RULE. An engagement is a run of blocks in which one room
        is selected and its network runs.
          A block that wants room R while the stage holds no room starts an
          engagement: R's settings, the network emptied, and a fade in from
          the dry signal. The emptied network has no tail to fade; the fade is
          for the dry, which the engine turns down by its mix.
          A block that does not want the held room fades it out, still running
          the network on what arrives, and when the fade reaches zero the
          engagement is over and nothing is held. A room wanted again before
          that fade ends continues its engagement, fading back up from where
          the fade had got to: its tail is milliseconds old, not a replay.
          Room A to room B is the same: A fades out, is let go, and B engages
          on the next block with an emptied network.
        So no tail is ever heard from an earlier engagement except in the 30 ms
        fade that ends it.

        THE IDLE-GAP RULE. The plugin reports no tail (getTailLengthSeconds
        returns 0.0, PluginProcessor.h), so a host may stop calling processBlock
        on an idle channel. The selection never changes, so the rule above sees
        one engagement, and the tail frozen in the network would resume minutes
        later. So: if more time has passed since the last block that ran the
        room than the room's tail plus that block's own length, the network is
        emptied. A real room's tail would have died in that time anyway. No fade:
        the last sample this stage put out was that long ago. A fade-out
        interrupted by such a gap simply ends.

        THE ALIASED POINTER. For a one-channel buffer the call site passes
        getWritePointer(0) twice. The engine treats any non-null right channel
        as real and writes wetR into it, which here would be written over the
        left channel's output. So a right channel that is the left one goes to
        the engine as nullptr, the test runVoicing already makes (right != left),
        and the engine then runs from the one channel. pr PIN6. */
    bool runRoom (echojay::PlaybackRoom wanted, float* const* channels,
                  int numChannels, int numSamples, double nowSeconds) noexcept
    {
        using echojay::PlaybackRoom;

        // Nothing held and nothing wanted: the room does not exist this block.
        // (roomPos_ is 0 whenever nothing is held.)
        if (roomHeld_ == PlaybackRoom::None && wanted == PlaybackRoom::None)
            return false;
        if (numChannels < 1 || channels == nullptr || channels[0] == nullptr)
            return false;

        float* const left  = channels[0];
        float* const right = (numChannels >= 2 && channels[1] != left) ? channels[1] : nullptr;

        // THE IDLE GAP: time the host did not show this stage.
        if (roomHeld_ != PlaybackRoom::None && lastRoomSeconds_ >= 0.0
            && nowSeconds - lastRoomSeconds_ > roomTailSeconds() + lastRoomBlockSec_)
        {
            room_.reset();   // the same room's settings: open list 195 cannot bite here
            if (wanted != roomHeld_)
            {
                roomHeld_ = PlaybackRoom::None;
                roomPos_  = 0;
            }
        }

        if (roomHeld_ == PlaybackRoom::None)
        {
            if (wanted == PlaybackRoom::None)
            {
                lastRoomSeconds_ = -1.0;
                return false;   // a gap ended a fade-out: nothing left to play
            }

            // THE ENGAGEMENT RESET.
            //
            // OPEN LIST 195, NOT FIXED HERE: ReverbEngine::reset() snaps the
            // lines to the lengths its last recompute() derived, and recompute()
            // is private, run only by prepare() and by process() on its dirty
            // flag. So a room whose settings differ from the last ones the
            // engine derived would engage at the OLD room's lengths and glide to
            // its own over the engine's 100 ms smoother, smearing the pitch of
            // its opening tail. The fix is in EedReverbEngine.h and reaches the
            // shipping EchoJay Reverb plugin, so it is not made from here.
            //
            // WITH ONE ROOM IT CANNOT BITE: prepare() applies the bedroom's
            // settings before the engine's own prepare() derives the lengths,
            // and nothing else is ever applied, so these setters change nothing
            // and reset() snaps to the bedroom's lengths. THE SECOND ROOM BRINGS
            // IT BACK, and the commit that adds one must deal with 195 first.
            //
            // THE ZERO ITSELF is 320 KB of std::fill on the audio thread at
            // 48 kHz, once per engagement, estimated at tens of microseconds
            // and never timed: open list 196.
            applyRoomSettings (wanted);
            room_.reset();
            roomHeld_ = wanted;
            roomPos_  = 0;
        }

        const int target = (wanted == roomHeld_) ? roomFadeLen_ : 0;
        bool wrote = false;

        for (int start = 0; start < numSamples; start += kRoomChunkSamples)
        {
            // THE FADE-OUT HAS ENDED: the engagement is over, and the rest of
            // the block is left exactly as it arrived.
            if (roomPos_ == 0 && target == 0)
                break;

            const int n = std::min (kRoomChunkSamples, numSamples - start);
            float* const l = left + start;
            float* const r = (right != nullptr) ? right + start : nullptr;

            std::copy (l, l + n, dryL_);
            if (r != nullptr) std::copy (r, r + n, dryR_);

            room_.process (l, r, n);
            wrote = true;

            // FULLY ENGAGED: the network's output, exactly as it produced it.
            if (roomPos_ == roomFadeLen_ && target == roomFadeLen_)
                continue;

            // THE FADE, per sample. At 0 the dry exactly, at the full length the
            // network's output exactly, and a straight line between: the first
            // sample of an engagement is the dry, and the first sample after a
            // room is left is the one it would have played had it stayed.
            for (int i = 0; i < n; ++i)
            {
                if (roomPos_ == 0)
                {
                    l[i] = dryL_[i];
                    if (r != nullptr) r[i] = dryR_[i];
                }
                else if (roomPos_ < roomFadeLen_)
                {
                    const float g = (float) roomPos_ / (float) roomFadeLen_;
                    l[i] = dryL_[i] + g * (l[i] - dryL_[i]);
                    if (r != nullptr) r[i] = dryR_[i] + g * (r[i] - dryR_[i]);
                }
                roomPos_ += (roomPos_ < target) ? 1 : (roomPos_ > target ? -1 : 0);
            }
        }

        if (roomPos_ == 0 && target == 0)
            roomHeld_ = PlaybackRoom::None;

        lastRoomSeconds_  = (roomHeld_ == PlaybackRoom::None) ? -1.0 : nowSeconds;
        lastRoomBlockSec_ = (double) std::max (0, numSamples) / sampleRate_;
        return wrote;
    }

    PlaybackSimSelection     selection_;
    echojay::VoicingChain    chains_[2];
    echojay::PlaybackVoicing previousVoicing_ = echojay::PlaybackVoicing::None;

    // ---- the room: audio thread only, apart from prepare() -------------------
    echojay::ReverbEngine    room_;
    echojay::PlaybackRoom    roomHeld_  = echojay::PlaybackRoom::None;  // whose signal the network holds
    int                      roomPos_   = 0;   // fade position, 0 to roomFadeLen_: the network's share
    int                      roomFadeLen_ = 1;
    double                   sampleRate_  = 44100.0;
    double                   lastRoomSeconds_  = -1.0;   // clock at the last block that ran the room
    double                   lastRoomBlockSec_ = 0.0;    // and that block's length
    double                   ticksToSeconds_   = 1.0e-9;
    float                    dryL_[kRoomChunkSamples] {};
    float                    dryR_[kRoomChunkSamples] {};
};

/** THE SELECTION'S BODY: one active selection, mapped to what it runs. True
    when the body ran, false when it could not (the guards) or when the value
    has no case (the trailing false, which pb PIN7 exists to find). The room's
    network is not a body: applyPlaybackSim runs it after this, for every
    selection, because a room being left keeps fading under whatever comes next. */
inline bool playbackSimBody (PlaybackSimStage& stage, PlaybackSim s, float* const* channels,
                             int numChannels, int numSamples) noexcept
{
    switch (s)
    {
        case PlaybackSim::MonoFold:
        {
            // The fold runs no voicing, so for the reset rule this block is
            // "none", exactly as if nothing were selected. Everything below
            // this line is the fold as it was: unfiltered, and bit-identical.
            stage.previousVoicing_ = echojay::PlaybackVoicing::None;

            // THE GUARDS. Each returns false having written nothing, because
            // under applyPlaybackSim's contract false means the body did not run.
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

        // THE ROOMS. The body is the room's source device, or nothing for a
        // room with none; the network runs in applyPlaybackSim, after this.
        // The club floor runs the club PA and the festival field runs the same
        // system heard from a distance, so their bodies fold to mono first and
        // carry that voicing's filters, under the voicing rule, while their
        // networks follow the room's. THE ORDER IS WHAT MAKES THE FESTIVAL
        // FIELD POSSIBLE: the source is filtered on its way INTO the room, so
        // its low pass is the air over the distance, which the reverb itself
        // could not model.
        case PlaybackSim::Bedroom:
            return stage.runRoomSource (echojay::PlaybackRoom::Bedroom,
                                        channels, numChannels, numSamples);
        case PlaybackSim::SmallBar:
            return stage.runRoomSource (echojay::PlaybackRoom::SmallBar,
                                        channels, numChannels, numSamples);
        case PlaybackSim::ClubFloor:
            return stage.runRoomSource (echojay::PlaybackRoom::ClubFloor,
                                        channels, numChannels, numSamples);
        case PlaybackSim::FestivalField:
            return stage.runRoomSource (echojay::PlaybackRoom::FestivalField,
                                        channels, numChannels, numSamples);

        case PlaybackSim::None:
        case PlaybackSim::Count:
            // None never reaches here (applyPlaybackSim does not call the body
            // for it) and Count is a sentinel, not a selection. Both are listed
            // so -Wswitch-enum stays quiet WITHOUT a default label: a default
            // would swallow a newly added value silently, and the whole
            // arrangement depends on a new value being visible rather than
            // absorbed.
            break;
    }

    // REACHED ONLY BY A VALUE WITH NO CASE ABOVE, which is the failure pb PIN7
    // is built to catch. An active selection that arrives here has run nothing,
    // so false is the honest answer: the body did not execute because there is
    // no body. A new environment added without a case falls through to exactly
    // this line, and the sweep says which value did it.
    return false;
}

/** The stage itself: samples in place, or nothing at all.

    REAL-TIME SAFE, and it has to stay that way. No allocation, no locks, no
    file access, no logging. The voicings' filter state and the room's network
    live in the stage: they are zeroed in prepareToPlay
    (PlaybackSimStage::prepare) and otherwise touched only here, on the audio
    thread. The selection is read ONCE per call, so a block runs one selection
    from its first sample to its last. nowSeconds is the processor's clock,
    PlaybackSimStage::clockSeconds, for the room's idle-gap rule.

    THE CONTRACT CHANGED ON 19 SEP 2026, WITH THE ROOMS, AND THIS IS WHAT IS NOW
    TRUE. Before, the return meant the selection's body ran, and it was false
    whenever nothing was selected, from the very next block. A room cannot keep
    that promise without clicking: switched off mid-tail, it fades out over
    30 ms, and during that fade the stage runs and writes the buffer although
    nothing is selected.

    THE RETURN VALUE MEANS THE STAGE RAN SOMETHING OVER THIS BLOCK: the
    selection's body, or a room's network, including a room still fading out
    after the selection left it. False: it ran nothing, and the buffer is
    bit-identical to what arrived.

    IT IS FALSE when nothing is selected AND no room is fading out, or when the
    buffer has too few channels for the selected simulation to be defined. So
    after a room is switched off it is true for 30 ms more (1,440 samples at
    48 kHz, rounded to the blocks that contain them), and false from the first
    block after the fade has ended. After anything that is not a room, off is
    still off from the very next block. pb PIN1 holds the first half of that
    and pr PIN4 the second.

    IT IS NOT A CLAIM THAT ANY SAMPLE CHANGED, and reading it as one will
    mislead you. A mono fold applied to channels that already hold identical
    audio runs in full and leaves every sample bit-identical. It returns true,
    because it ran; the buffer is untouched, because there was nothing to
    change. Tying the return to sample values would make it a function of
    CONTENT rather than of selection, and then no fixed test signal could
    establish whether the stage executed.

    THE NO-OP IS NOT PINNED BY THIS VALUE. pb PIN1 and pr PIN4 copy the buffer
    and compare it with memcmp afterwards, because the memcmp READS THE BUFFER
    while this return only comments on it. A return value that lied would pass
    a test built on the return value. */
inline bool applyPlaybackSim (PlaybackSimStage& stage, float* const* channels,
                              int numChannels, int numSamples, double nowSeconds) noexcept
{
    const PlaybackSim s = stage.selection_.get();

    // THE BODY, or for nothing selected the one thing the early-out ever
    // wrote, and never to the buffer: the record that this block ran no
    // voicing, so the next voiced block is an activation and starts from
    // zeroed filters (pb PIN10).
    bool ran = false;
    if (playbackSimActive (s))
        ran = playbackSimBody (stage, s, channels, numChannels, numSamples);
    else
        stage.previousVoicing_ = echojay::PlaybackVoicing::None;

    // THE ROOM, after the body, for every selection: a room being engaged, a
    // room held, or a room fading out under whatever replaced it. With no room
    // held and none wanted it returns at once, having touched nothing.
    const bool roomRan = stage.runRoom (playbackSimRoom (s), channels, numChannels,
                                        numSamples, nowSeconds);
    return ran || roomRan;
}
