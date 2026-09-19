#pragma once

#include <array>
#include "EJPlaybackSim.h"   // PlaybackSim, and echojay::PlaybackVoicing through it

// =============================================================================
//  THE PLAYBACK GRID'S TILES: what each one is, in the order they are drawn.
// =============================================================================
//
// ONE TABLE, WALKED BY ONE HANDLER. The editor does not have a hit handler per
// tile: it finds the pressed tile's row here and either stores its selection
// or opens the render view. So the six live tiles share ONE store line, and a
// tile cannot lose its wiring without every tile losing it. pb PIN9 names
// that line and checks this table's order, which together cover all six; a
// per-tile handler would have needed a structural pin per tile to say as much.
//
// Pure data, no art. The pictures are looked up from a tile in
// EJPlaybackArtMap.h, which only the EchoJay target may include; this header
// has no such restriction, so mapfps_test can read the table the editor walks.

namespace echojay
{

enum class PlaybackTileKind
{
    Live,          ///< changes what is playing now: stores `sim`, a toggle back to None
    CodecRender    ///< opens the codec card as the render view; plays and stores nothing
};

struct PlaybackTile
{
    PlaybackTileKind kind;
    PlaybackSim      sim;       ///< what a Live tile stores; None for the codec tile
    PlaybackVoicing  voicing;   ///< whose picture a voicing tile shows; None for Mono and codec
    const char*      label;     ///< the label band's text, sized to fit the smallest tile
};

/** Ten live tiles, then the codec tile, in drawing order. The live tiles run
    in PlaybackSim's own order, the Mono tile first; pb PIN9 holds that.

    A ROOM TILE CARRIES ITS ROOM'S SOURCE as its voicing: None for the bedroom,
    which is the room alone. Its picture is matched on its room, not on that
    voicing (playbackArtForTile).

    THE CODEC TILE'S LABEL SAYS IT RENDERS. "Render a codec": 92.9 px in the
    system bold face at 12 pt, the wider of the two faces the LookAndFeel can
    fall back to (DM Sans is not installed everywhere), inside the 116 px a
    132 px tile leaves after its 8 px insets (132 since the scrollbar's gutter,
    19 Sep 2026; it was 133). The longest live label is now "Bluetooth
    speaker", 110.1 px, measured the same way (19 Sep 2026): 5.9 px to spare.
    "TV soundbar" is 76.9 px and "Club PA" 47.2 px. "Bedroom" is 54.5 px, measured the same way.

    ELEVEN TILES SCROLL. At the smallest page the grid shows two whole rows,
    eight tiles, so the last three, Club PA, Bedroom and the codec tile, sit in
    the partly visible third row until the grid is scrolled, and the scrollbar
    at the grid's right edge says there is more. Eleven is still three rows of
    four, so no grid figure moved from ten. */
inline constexpr std::array<PlaybackTile, 11> kPlaybackTiles {{
    { PlaybackTileKind::Live,        PlaybackSim::MonoFold,     PlaybackVoicing::None,         "Mono" },
    { PlaybackTileKind::Live,        PlaybackSim::PhoneSpeaker, PlaybackVoicing::PhoneSpeaker, "Phone speaker" },
    { PlaybackTileKind::Live,        PlaybackSim::Laptop,       PlaybackVoicing::Laptop,       "Laptop" },
    { PlaybackTileKind::Live,        PlaybackSim::CarDashboard, PlaybackVoicing::CarDashboard, "Car" },
    { PlaybackTileKind::Live,        PlaybackSim::KitchenRadio, PlaybackVoicing::KitchenRadio, "Kitchen radio" },
    { PlaybackTileKind::Live,        PlaybackSim::Earbuds,      PlaybackVoicing::Earbuds,      "Earbuds" },
    { PlaybackTileKind::Live,        PlaybackSim::TvSoundbar,       PlaybackVoicing::TvSoundbar,       "TV soundbar" },
    { PlaybackTileKind::Live,        PlaybackSim::BluetoothSpeaker, PlaybackVoicing::BluetoothSpeaker, "Bluetooth speaker" },
    { PlaybackTileKind::Live,        PlaybackSim::ClubPA,           PlaybackVoicing::ClubPA,           "Club PA" },
    { PlaybackTileKind::Live,        PlaybackSim::Bedroom,          PlaybackVoicing::None,             "Bedroom" },
    { PlaybackTileKind::CodecRender, PlaybackSim::None,         PlaybackVoicing::None,         "Render a codec" },
}};

} // namespace echojay
