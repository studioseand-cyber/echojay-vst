#pragma once

// =============================================================================
//  PLAYBACK ROOMS: a space the mix is played into, by the reverb network.
// =============================================================================
//
// A ROOM IS NOT A DEVICE, so it has a table of its own rather than nine more
// fields on VoicingRow. A device row is filter coefficients; a room row is the
// reverb network's settings plus the device that plays into the room. Putting
// both in one row would give every device row nine meaningless values, and
// aggregate initialisation fills a missing field with zero silently: for a
// filter that zero reddens pv PIN1, but for a reverb it plays (a decay of 0 is
// clamped to 0.1 s, a mix of 0 is quietly off). So each room row is written out
// in full, and pr PIN1 checks every field against the engine's own ranges.
//
// THE ENGINE is echojay::ReverbEngine (EedReverbEngine.h), the network behind
// the EchoJay Reverb plugin, used here unchanged. How the stage engages it,
// fades it and empties it is PlaybackSimStage's business, in EJPlaybackSim.h.

#include <array>
#include <cstddef>
#include "EJPlaybackVoicing.h"   // echojay::PlaybackVoicing: a room's source device
#include "EedReverbEngine.h"     // echojay::ReverbAlgorithm, and the ranges pr PIN1 reads

namespace echojay
{

/** The rooms. None means "no room": the stage's reverb does not run.

    ONE ROOM IN THIS COMMIT, ON PURPOSE. The mechanism is judged on the simplest
    case, a room with no source device, before three more sets of chosen
    numbers arrive with small_bar, club_floor and festival_field. */
enum class PlaybackRoom
{
    None = 0,
    Bedroom,

    /** SENTINEL, ALWAYS LAST. The table is sized by it and pr PIN1 sweeps up
        to it, so a value added below Count would have no row and no pin. */
    Count
};

/** One room: what plays into it, and the network's settings for the space.

    CARRIED, because they are what makes one space differ from another: the
    algorithm, size, decay, predelay, damping, the tail's low cut, the
    early/late balance, diffusion, and how much of the room is heard (mix).

    NOT CARRIED, left at the engine's own defaults, and said here so their
    absence is a decision rather than an omission:
      mod depth   25%, there to smear the network's modes, not to describe a
                  space (EedReverbEngine.h, at modSamples);
      duck        0%, a production effect: a room does not duck;
      width       100%, "the network's own stereo", which is the room's.
    The stage never calls their setters, so the engine's defaults stand. */
struct RoomRow
{
    /** The device that plays into the room, run through the voicing chains
        before the reverb, under the voicing reset rule. None means the room
        alone: the mix as it is, played into the space. */
    PlaybackVoicing source;

    ReverbAlgorithm algorithm;
    float sizePct;        ///< ReverbEngine::setSizePct
    float decaySec;       ///< RT60 asked for; damping shortens what is heard
    float predelayMs;
    float dampingPct;
    float lowCutHz;       ///< inside the network's lines: the TAIL's low cut, not the dry's
    float earlyLatePct;   ///< before the algorithm's bias, which the engine adds
    float diffusionPct;
    float mixPct;         ///< how much of the room is heard against the direct sound
};

/** THE NUMBERS ARE CHOSEN, NOT MEASURED, like every row of the voicing table.
    Nobody has measured a bedroom for this. The only real test is listening
    in one.

    BEDROOM (19 Sep 2026). Source None: Kathy's decision, the room only, the
    mix as it is played into the space.
      algorithm  Room: short dense lines, a tight early cluster; "you hear the
                 walls" (EedReverbEngine.h, the algorithm table).
      size 20%   the engine reads this as about 3.85 m ((3 + 20 * 0.20) * 0.55,
                 roomSizeMetres), a small bedroom.
      decay 0.4 s  a furnished bedroom's RT60 is short. With the damping below,
                 what is heard is 0.4 * (1 - 0.45 * 0.6) = 0.29 s
                 (effectiveDecaySeconds).
      predelay 3 ms  walls a metre or two away: the first reflection is close
                 behind the direct sound.
      damping 60%  bed, curtains, carpet: the tail darkens fast. The corner is
                 about 2.9 kHz (dampingCutoffHz at 60%, times the Room
                 algorithm's 1.05).
      low cut 100 Hz  the tail's, not the mix's: the dry passes untouched.
      early/late 55%  a small room is mostly early reflections. The Room
                 algorithm biases this by -0.28, so the late share heard is 27%.
      diffusion 75%  kReverbDiffusionUnityPct: the network's own allpass gains.
      mix 25%    the room is heard, and the mix still leads it.
    Its tail, by the Reverb plugin's formula, is 0.003 + 0.29 * 1.2 = 0.35 s:
    the idle-gap rule's threshold for this room.

    Indexed by PlaybackRoom. The None row is never read: None engages nothing.
    pr PIN1 is what catches a row of zeros. */
inline constexpr std::array<RoomRow, (std::size_t) PlaybackRoom::Count> kRoomTable {{
    //               source                 algorithm              size  decay pre   damp  lowcut e/l   diff  mix
    /* None    */ { PlaybackVoicing::None, ReverbAlgorithm::Hall,  0.0f, 0.0f, 0.0f, 0.0f,   0.0f, 0.0f, 0.0f, 0.0f },
    /* Bedroom */ { PlaybackVoicing::None, ReverbAlgorithm::Room, 20.0f, 0.4f, 3.0f, 60.0f, 100.0f, 55.0f, 75.0f, 25.0f },
}};

/** The row for room r, or the None row for None, Count and anything outside
    them, so a bad cast can never index past the table. */
inline const RoomRow& roomRow (PlaybackRoom r) noexcept
{
    const int i = (int) r;
    if (i <= (int) PlaybackRoom::None || i >= (int) PlaybackRoom::Count)
        return kRoomTable[0];
    return kRoomTable[(std::size_t) i];
}

} // namespace echojay
