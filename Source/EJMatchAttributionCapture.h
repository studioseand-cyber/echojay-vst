#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <optional>
#include <vector>
#include "EJMatchAttribution.h"   // AttribIn, attribSeriesWindow, matchBusTapOf, the routing

// =============================================================================
//  K5: ATTRIBUTION FROM A REAL CAPTURE. One AttribIn per band, read off the
//  capture's own series, ready for attributeBandMove.
// =============================================================================
//
// C4 rev 4 section 2 and C2 sections 5.1 and 5.2. For each band with a gap:
//   - P_i: each Link's channel series (ChannelMeterData::hopFrames) over the
//     window, with that Link's trim AS IT WAS AT THE CAPTURE'S START
//     (ChannelMeterData::gainDb) added back by attributeBandMove;
//   - P_bus: the rack INPUT series (hopFramesRackIn) when the capture has one,
//     pre-chain and pre-trim, read as it is; otherwise the bus OUTPUT
//     (hopFrames) with busGainDbAtCapture added back, its chain included;
//   - busTap: matchBusTapOf (the capture), so rule 3's sentence and the
//     payload say which.
//
// THE GRID CHECK READS WHOLE SERIES, NOT THE WINDOW. C4 section 2: a Link whose
// hop count differs from the bus's by more than one has fallen off the grid.
// Both counts are the series' own lengths (ChannelMeterData::hops and the
// bus's hopFrames(RackIn)Hops), whatever window is being read.
//
// ROLES COME FROM THE HOST. ChannelMeterData carries no role; the caller hands
// over MatchHost::liveLinks(), and a Link it does not list has an empty role,
// which C4 4a reads as able to carry any band (the generous reading).
//
// THE HOST'S OWN CHANNEL IS NOT A LINK. A channel with an empty uid is the
// host's (ChannelMeterData::uid's own comment) and is skipped.
//
// PURE. A template over the capture and the link list, so the gate drives it
// with the real CaptureSnapshot and MatchLink, and nothing here names either.
// =============================================================================

namespace echojay
{

/** The bus side over a window: power per band (pre-trim), its hop count, and
    where it was read. */
struct AttribBusFromCapture
{
    std::array<double, 6> power {};
    int hopCount = 0;
    MatchBusTap tap = MatchBusTap::Output;
};

template <typename Capture>
inline AttribBusFromCapture attribBusFromCapture (const Capture& c, HopRange w)
{
    AttribBusFromCapture b;
    b.tap = matchBusTapOf (c);
    const bool in = b.tap == MatchBusTap::Input;
    const auto& series = in ? c.hopFramesRackIn : c.hopFrames;
    const auto win = attribSeriesWindow (series, w);
    for (std::size_t k = 0; k < 6; ++k)
        b.power[k] = in ? win.power[k] : attribPreTrimPower (win.power[k], c.busGainDbAtCapture);
    b.hopCount = in ? c.hopFramesRackInHops : c.hopFramesHops;
    return b;
}

/** One AttribIn per band that has a gap (D, mix minus reference, dB). A band
    with no gap is nullopt: there is nothing to place. */
template <typename Capture, typename Link>
inline std::array<std::optional<AttribIn>, 6>
    attribInsFromCapture (const Capture& c, HopRange w,
                          const std::array<std::optional<float>, 6>& D,
                          const std::vector<Link>& links)
{
    std::array<std::optional<AttribIn>, 6> out {};
    const auto bus = attribBusFromCapture (c, w);

    struct Ch { juce::String uid, name, role; AttribSeriesWindow win; float gainDb; int hops; };
    std::vector<Ch> chans;
    for (const auto& ch : c.channels)
    {
        if (ch.uid.isEmpty()) continue;
        juce::String role;
        for (const auto& l : links) if (l.uid == ch.uid) { role = l.role; break; }
        chans.push_back ({ ch.uid, ch.name, role, attribSeriesWindow (ch.hopFrames, w), ch.gainDb, ch.hops });
    }

    for (int b = 0; b < 6; ++b)
    {
        if (! D[(std::size_t) b]) continue;
        AttribIn in;
        in.band = b;
        in.busPower = bus.power[(std::size_t) b];
        in.busHopCount = bus.hopCount;
        in.D = *D[(std::size_t) b];
        in.busTap = bus.tap;
        for (const auto& ch : chans)
        {
            AttribLinkIn l;
            l.uid = ch.uid; l.name = ch.name; l.role = ch.role;
            l.postTrimPower = ch.win.power[(std::size_t) b];
            l.gainDb = ch.gainDb;
            l.hopCount = ch.hops;
            l.frameCoverage = ch.win.frameCoverage;
            in.links.push_back (l);
        }
        out[(std::size_t) b] = in;
    }
    return out;
}

} // namespace echojay
