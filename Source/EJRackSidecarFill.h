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
        // SEAN'S #1 (16 Oct 2026): the LANDED numbers, so the agent's read-back on a Link reads what the
        // plugin shows instead of parsing them back out of an echo of its own request.
        back.landedValues = s.landedValues;
        // ---- AND THE CARD IS COMPOSED FROM THAT LEDGER, HERE (17 Oct 2026, Sean's step 4) -------
        // The mirror showed "Threshold 0" beside a ledger that said -30, twice, on the Link road -
        // and no log from a Link's process reaches the runner's file, so this is fixed at the
        // boundary rather than diagnosed from outside it. Whatever wrote the slot's own card, the
        // text that LEAVES here is composed from the numbers that leave with it, by the one
        // function that composes them. Nothing to contradict.
        if (const auto fromLedger = ChainHost::cardTextFromLandedValues (s.landedValues);
            fromLedger.isNotEmpty())
            back.settings = fromLedger.substring (0, 200);
        // 16 Oct 2026 (the joint live pass): whether this slot's dial has FINISHED, so the agent's
        // read-back on a Link can wait for the round trip instead of calling an unread value a
        // failed write.
        back.dialPending  = s.dialPending;
        // ---- ITEM 13: THIS SLOT'S MEASUREMENTS (18 Oct 2026, Sean) ------------------------------
        //
        // *"On the Kick's Link, check gr fails with 'Gain reduction cannot be read from a Link yet',
        // so the certified-compressor measurement can't run on any Link."*
        //
        // The Link has measured them all along - it runs this same ChainHost - and slotPicture is the
        // one place that composes them, with its own four gates (a group is a reading only when it
        // HAS one; the dB fields sit at a no-reading floor otherwise). Copied field for field, so the
        // Link publishes exactly what the main plugin would read from its own rack, and no reader has
        // to decide anything the picture has already decided.
        // ---- WHAT THIS SLOT HEARD (18 Oct 2026) ------------------------------------------------
        // The per-slot tallies are live for EVERY slot, third party included - they are the one thing
        // EchoJay measures about a plugin whose meters it cannot read. p90/p50 over the gated 100 ms
        // hops, and the same two percentiles of those hops' true peaks.
        {
            const auto lv = host.getSlotLevels (i);
            if (lv.measured && lv.in.known)
            {
                back.inLevelsKnown     = true;
                back.inLevelsKWeighted = lv.in.kWeighted;
                back.inLoudestDb       = lv.in.p90;
                back.inTypicalDb       = lv.in.p50;
                // 19 Oct 2026: and the hop percentiles, taken by the Snapshot's own implementation
                // so this side and the open rack's cannot take them differently.
                back.inRmsKnown        = lv.in.hopLevelPercentiles (back.inTypicalRmsDb, back.inLoudestRmsDb);
                back.inRmsSeconds      = lv.in.hopLevelSeconds();
                back.dialConfirmed     = host.slotDialConfirmedLabels (i);
                back.outRmsKnown       = lv.out.hopLevelPercentiles (back.outTypicalRmsDb, back.outLoudestRmsDb);
                back.inCrestKnown      = lv.in.hopCrest  (back.inCrestDb);
                back.outCrestKnown     = lv.out.hopCrest (back.outCrestDb);
                back.inWindowSeconds   = lv.in.windowSeconds;
                juce::Array<float> hops;
                for (int k = 0; k < lv.in.hopTruePeakCount && k < (int) lv.in.hopTruePeakDb.size(); ++k)
                    if (lv.in.hopTruePeakDb[(size_t) k] > -199.0f) hops.add (lv.in.hopTruePeakDb[(size_t) k]);
                if (! hops.isEmpty())
                {
                    hops.sort();
                    back.inTypicalPeakDbtp = hops[hops.size() / 2];
                    back.inLoudestPeakDbtp = hops[juce::jmin (hops.size() - 1, (hops.size() * 9) / 10)];
                }
                else if (lv.in.truePeakDb > -199.0f)
                    back.inLoudestPeakDbtp = back.inTypicalPeakDbtp = lv.in.truePeakDb;
            }
        }
        const auto pic = host.slotPicture (i);
        back.pictureValid  = pic.valid;
        back.grKnown       = pic.grKnown;
        back.grCertified   = pic.grCertified;
        back.grDb          = pic.grDb;
        back.grPeakDb      = pic.grPeakDb;
        back.grTypicalDb   = pic.grTypicalDb;
        back.grLoudestDb   = pic.grLoudestDb;
        back.grHeldDb      = pic.grHeldDb;
        back.grWindowFrames = (double) pic.grWindowFrames;
        back.inLufs        = pic.inLufs;
        back.outLufs       = pic.outLufs;
        back.inTpDb        = pic.inTpDb;
        back.outTpDb       = pic.outTpDb;
    }
    // THIS BUILD PUBLISHES THEM, AND WHEN. The stamp is what lets the reader tell a live reading
    // from the window this Link happened to have ten minutes ago - and quoting the latter would be
    // an estimate wearing a measurement's clothes, which is the one thing his 15 Oct ruling forbids.
    rc.meterCapable = true;
    rc.measuredAtMs = juce::Time::currentTimeMillis();
}
} // namespace echojay
