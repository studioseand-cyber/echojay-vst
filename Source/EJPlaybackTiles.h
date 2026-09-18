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

/** Six live tiles, then the codec tile, in drawing order.

    THE CODEC TILE'S LABEL SAYS IT RENDERS. "Render a codec": 92.9 px in the
    system bold face at 12 pt, the wider of the two faces the LookAndFeel can
    fall back to (DM Sans is not installed everywhere), inside the 117 px a
    133 px tile leaves after its 8 px insets. The longest live label,
    "Phone speaker", is 89.0 px. */
inline constexpr std::array<PlaybackTile, 7> kPlaybackTiles {{
    { PlaybackTileKind::Live,        PlaybackSim::MonoFold,     PlaybackVoicing::None,         "Mono" },
    { PlaybackTileKind::Live,        PlaybackSim::PhoneSpeaker, PlaybackVoicing::PhoneSpeaker, "Phone speaker" },
    { PlaybackTileKind::Live,        PlaybackSim::Laptop,       PlaybackVoicing::Laptop,       "Laptop" },
    { PlaybackTileKind::Live,        PlaybackSim::CarDashboard, PlaybackVoicing::CarDashboard, "Car" },
    { PlaybackTileKind::Live,        PlaybackSim::KitchenRadio, PlaybackVoicing::KitchenRadio, "Kitchen radio" },
    { PlaybackTileKind::Live,        PlaybackSim::Earbuds,      PlaybackVoicing::Earbuds,      "Earbuds" },
    { PlaybackTileKind::CodecRender, PlaybackSim::None,         PlaybackVoicing::None,         "Render a codec" },
}};

} // namespace echojay
