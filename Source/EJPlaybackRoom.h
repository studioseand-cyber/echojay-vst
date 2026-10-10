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

    THE OTHER THREE ARRIVED ON 20 SEP 2026, once the mechanism had been judged
    on the simplest case. Two of them have a source device and two do not,
    which is the distinction the source field exists for. */
enum class PlaybackRoom
{
    None = 0,
    Bedroom,
    SmallBar,
    ClubFloor,
    FestivalField,

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

    SMALL BAR (20 Sep 2026). Source None, Kathy's answer again: the room only.
      algorithm  Room, the same small-space network as the bedroom.
      size 50%   about 7.15 m ((3 + 20 * 0.50) * 0.55): a room crossed in a
                 few steps, not a hall.
      decay 0.8 s  hard surfaces, glass and bottles and a wooden floor. What
                 is heard after the damping is 0.67 s.
      predelay 8 ms  the walls are further off than a bedroom's.
      damping 35%  brighter than the bedroom: the corner is about 6.4 kHz.
      low cut 80 Hz  the tail's, not the mix's.
      early/late 65%  the Room algorithm's -0.28 bias leaves 37% late: more
                 diffuse field than the bedroom, still reflection-led.
      diffusion 75%, mix 30%.
      Its tail is 0.008 + 0.67 * 1.2 = 0.82 s.
      NOT MODELLED: people. A full bar is a different room from an empty one,
      and the absorption of bodies is not in these numbers.

    CLUB FLOOR (20 Sep 2026). Source ClubPA, COMPOSED RATHER THAN COPIED: the
    PA is what the room reflects, so the club system's own numbers stay in one
    place, kVoicingTable's ClubPA row, and this row is the space around it.
    Copying them here would be two tables that agree today, which is why
    monoFoldInPlace became one function. It inherits ClubPA's monoFirst, which
    is right: the system sums to mono and the room then decorrelates what it
    hears, from the first early reflection.
      algorithm  Hall: long lines, the diffuse field of a big dark space.
      size 80%   about 19 m ((3 + 20 * 0.80) * 1.00).
      decay 1.6 s  concrete and steel, loaded with bodies and rigging. What
                 is heard after the damping is 1.20 s.
      predelay 18 ms  the far walls.
      damping 55%  the corner is about 3.2 kHz: a club's tail is dark.
      low cut 60 Hz  the tail keeps its low end, which is what a club sounds
                 like from the floor.
      early/late 75%  the diffuse field leads, not the first reflections.
      diffusion 85%  dense.
      mix 35%    you are in the room with it.
      Its tail is 0.018 + 1.20 * 1.2 = 1.46 s.
      NOT MODELLED: the crowd, and the system's own delay towers.

    FESTIVAL FIELD (20 Sep 2026, rewritten the same day: see below). Source
    FestivalPA, NOT ClubPA, and a room that is far away rather than small.

      THE DISTANCE IS MODELLED, AND IT IS IN THE VOICING. This row composed
      from ClubPA at first and made a PA with the reflections taken away, which
      is not what a field sounds like: what you hear out there is air
      swallowing the top end over the distance to the stage. The reverb cannot
      do that (its damping is inside the feedback loop, so it darkens the tail
      only, and its dry path has no filter), but the SOURCE VOICING CAN,
      because the stage runs it before the room. So the distance lives in
      FestivalPA's row in EJPlaybackVoicing.h: the club system's shape with its
      top rolled off from 6.5 kHz, about 9.5 dB down at 10 kHz, and its bottom
      untouched. It keeps the mono fold.

      THE ROOM IS THE OTHER HALF. Outdoors nothing stands near you, so there is
      almost nothing to reflect early; what comes back arrives late, from far
      away, and dark.
      algorithm  Hall: long lines and no bias of its own, so the balance below
                 is the balance heard.
      size 70%   about 17 m of geometry ((3 + 20 * 0.70) * 1.00): the distant
                 returns, not a room around you.
      decay 2.0 s  a long, low return rather than a small space's ring. What
                 is heard after the damping is 1.33 s.
      predelay 75 ms  THE REASON THIS READS AS OUTDOORS: nothing is close, so
                 the first thing that comes back is late. Sixty to a hundred
                 milliseconds is the range; 75 is the chosen number.
      damping 75%  the corner is about 1.7 kHz: what returns is dull, because
                 it has been through the same air twice.
      early/late 85%  biased LATE, the opposite of the small rooms: the early
                 cluster is nearly absent outdoors.
      low cut 120 Hz  the TAIL's: outdoors there is no room gain to carry the
                 bottom, though the direct sound keeps its sub (see the
                 voicing).
      diffusion 45%  sparse, not a cloud: a field has few surfaces.
      mix 15%    still low: mostly the PA, direct.
      Its tail is 0.075 + 1.33 * 1.2 = 1.67 s.
      WHAT IS STILL NOT MODELLED: the wind, the system's delay towers, and the
      crowd.

    Indexed by PlaybackRoom. The None row is never read: None engages nothing.
    pr PIN1 is what catches a row of zeros. */
inline constexpr std::array<RoomRow, (std::size_t) PlaybackRoom::Count> kRoomTable {{
    //                     source                   algorithm                  size   decay  pre    damp   lowcut  e/l    diff   mix
    /* None          */ { PlaybackVoicing::None,   ReverbAlgorithm::Hall,      0.0f,  0.0f,  0.0f,  0.0f,   0.0f,  0.0f,  0.0f,  0.0f },
    /* Bedroom       */ { PlaybackVoicing::None,   ReverbAlgorithm::Room,     20.0f,  0.4f,  3.0f, 60.0f, 100.0f, 55.0f, 75.0f, 25.0f },
    /* SmallBar      */ { PlaybackVoicing::None,   ReverbAlgorithm::Room,     50.0f,  0.8f,  8.0f, 35.0f,  80.0f, 65.0f, 75.0f, 30.0f },
    /* ClubFloor     */ { PlaybackVoicing::ClubPA, ReverbAlgorithm::Hall,     80.0f,  1.6f, 18.0f, 55.0f,  60.0f, 75.0f, 85.0f, 35.0f },
    /* FestivalField */ { PlaybackVoicing::FestivalPA, ReverbAlgorithm::Hall,  70.0f,  2.0f, 75.0f, 75.0f, 120.0f, 85.0f, 45.0f, 15.0f },
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
