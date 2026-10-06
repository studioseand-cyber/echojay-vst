#pragma once
#include "LinkShm.h"
#include "ChainHost.h"

// =============================================================================================================
//  RULING 1 (21s-b, 24 Sep 2026): ONE SOURCE FOR "WHAT IS IN THIS RACK".
//
//  The sidecar (rack-<uid>.json) was written only by LinkProcessor::publishRackSidecar, from the LINK's own
//  ChainHost. While the main plugin holds a rack lease, the Link's slots are parked and V2 builds into its
//  BORROWED copy - so the Link kept publishing an empty rack while the user watched a plugin land in it. On
//  24 Sep that cost a real edit: a build put UAD Teletronix LA-2 into the borrowed rack, and the next Apply was
//  refused with "guard=baseSlots-count base=1 [UAD Teletronix LA-2] live=0", because the preflight read the
//  sidecar and the preview had come from the borrowed host. The chat's [CURRENT CHAIN] block had the same
//  problem from the other side: "Link target live but sidecar missing/empty - channel declaration only".
//
//  The slot half of that file is now filled HERE, from whichever ChainHost actually holds the rack, so the Link
//  and the lease holder cannot describe the same rack differently. The Link keeps everything only it can know
//  (its own identity, mute/solo, lease flags, the EQ curve); V2 fills the rest from the borrowed host and writes
//  the file itself, before the lock is released.
// =============================================================================================================
namespace echojay
{
// The slot list and the rack-level values every publisher agrees on. `lastEditMs` is the publisher's own last
// local edit stamp; `eqSlot` and `curve` are the Link's to supply (V2 passes -1 and nothing).
inline void fillRackSidecarSlots (LinkShm::RackSidecar& rc,
                                  const ChainHost& host,
                                  juce::int64 lastEditMs,
                                  int eqSlot = -1,
                                  const std::vector<int16_t>& curve = {},
                                  std::function<bool (int)> slotLeased = {})
{
    rc.masterWet = host.getMasterWet();
    rc.revision  = host.getChainRevision();
    const auto infos = host.getAllSlotInfos();
    for (int i = 0; i < (int) infos.size(); ++i)
    {
        const auto& s = infos[(size_t) i];
        rc.slots.push_back ({ s.name, s.format,
                              s.settings.substring (0, 200),   // bound file size
                              s.bypassed, s.wet,
                              slotLeased ? slotLeased (i) : false,
                              i == eqSlot ? curve : std::vector<int16_t>{} });
        const auto id = host.getSlotIdentity (i);
        auto& back = rc.slots.back();
        back.fp = id.fp; back.uid = id.uid; back.version = id.version;
        back.lastEditMs   = lastEditMs;
        back.manufacturer = s.manufacturer;
        // 5 Oct 2026: the slot's own EchoJay gains, so a borrow can start where the rack actually is.
        back.outGainDb    = host.getSlotOutGainDb (i);
        back.preTrimDb    = host.getSlotPreTrimDb (i);
    }
}
} // namespace echojay
