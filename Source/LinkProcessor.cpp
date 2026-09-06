#include "LinkProcessor.h"
#include "EJStateRoot.h"   // 6 Sep 2026: every user-state path resolves through the isolatable root
#include <signal.h>   // kill(pid, 0): publisher liveness (C4b)
#include <unistd.h>
#include "EedLatencyLog.h"
#include "LinkEditor.h"
#include "LinkShm.h"
#include "FaderTaper.h"   // shared mixer-fader mute taper (P17)
#include "NativeClip.h"   // EchoJay_NSLog — chain-build diagnostics
#include "EedKeyDetectorProcessor.h"   // hosted-detector frame preference (Tier 1)

// Item-1 (Active persistence) diagnosis logging — on by default until the
// flip point is confirmed in the field; EJLinkState: lines in the monitor.
#ifndef ECHOJAY_LINK_STATE_DIAG
 #define ECHOJAY_LINK_STATE_DIAG 1
#endif

LinkProcessor::LinkProcessor()
    : AudioProcessor(BusesProperties()
          .withInput ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    // Per-instance identity (commands/acks/registry address; also the ring
    // file part for unnamed Links). Serialised in state; regenerated on
    // registry collision after track duplication.
    instanceUid_ = juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()).removeCharacters("-").substring(0, 10);
   #if ECHOJAY_NO_VISUAL_FFT
    // Build-configuration diagnostic, and this pass's content check: the
    // 4096-point VISUAL FFT is compiled out of the Link (nothing here reads
    // a spectrum). The 2048-point ANALYSIS FFT stays, so macroBandDb and
    // the published bandRel[6] are unaffected. Absent from the main
    // plugin's binary, which consumes the visual spectrum.
    EchoJay_NSLog("EJLinkMeter: visual FFT gated out of this build; "
                  "analysis FFT and bandRel unaffected");
   #endif
    startTimerHz(30); // 30Hz meter-frame publish (fast ballistics step at
                      // 10Hz was ~1.3 dB per sample); polls every 3rd tick
                      // (~100ms, the old cadence), heartbeat every 30th (1s)

    // Key detection (KEY_DETECTOR_SPEC.md §9, KEY_PRECONDITION_SPEC.md §5.1):
    // PASSIVE, duty-cycled — a committed 8 s pass roughly every 30 s, armed
    // by schedulePassiveKeyPass() only while the transport rolls and signal
    // is present. Not continuous mode: the key does not change four times a
    // second, and this Link has no wheel to animate (live chroma off). The
    // worker idles at a 250 ms wakeup between passes.
    keyEngine_.setContinuous(false);
    keyEngine_.setWindowSeconds(kKeyPassWindowS);
    keyEngine_.setLiveChromaEnabled(false);
    keyWorker_.startThread();

    // Mirror hosted chain latency into the host on EVERY chain change —
    // Link sits on parallel and phase-critical tracks, so this must track
    // build/add/remove/bypass exactly.
    chainHost.onChainChanged = [this]
    {
        updateChainLatency();
        // Same stale-hold problem as the main plugin: the Link's own meterEngine_
        // reads POST-rack (its mix-bus strip), so a rack change leaves its held
        // true peak / peak / overs describing audio the rack no longer produces.
        // Drop those holds; integrated LUFS / LRA keep accumulating. Message
        // thread signal only.
        meterEngine_.resetHolds();
    };
}

LinkProcessor::~LinkProcessor()
{
    stopTimer();
    // Audio thread guaranteed stopped before destructor
    releaseRegistrySlot();
    // CLEAN-EXIT HYGIENE (25 Aug 2026): the uid is per-launch, so this
    // instance's uid-keyed files are unreachable the moment it dies —
    // delete them here rather than leaving them for the reaper (crashes
    // still litter; the main's sweep is the backstop). structplan is NOT
    // deleted: a lingering journal is a rollback, and completion already
    // deletes it. NOT in releaseRegistrySlot — that also runs on live
    // re-claims mid-session.
    if (instanceUid_.isNotEmpty() && resolvedDir.isNotEmpty())
    {
        for (const auto& p : { "rack-" + instanceUid_ + ".json",
                               "racklock-" + instanceUid_ + ".json",
                               "lease-" + instanceUid_ + ".json",
                               "ctrl-cmd-" + instanceUid_ + ".json",
                               "ctrl-ack-" + instanceUid_ + ".json",
                               "chain-cmd-" + instanceUid_ + ".json",
                               "chain-ack-" + instanceUid_ + ".json" })
            juce::File(resolvedDir + p).deleteFile();
    }
    const juce::String ringPath = shmOpenedKey;   // full path, set at openRing
    closeRingNow();
    if (ringPath.isNotEmpty())
        juce::File(ringPath).deleteFile();
    LinkShm::closeRegistry(regMap, regFd);
    regMap = nullptr; regFd = -1;
}

// =============================================================================
//  juce::Timer
// =============================================================================
void LinkProcessor::timerCallback()
{
    // Heartbeat once per second (timer runs at 30Hz since v0.8.5; only the
    // meter publish uses the full rate)
    // Solo fabric scan at ~4Hz: cheap by construction (shared-memory
    // registry walk; sidecar parses only when a file's mtime moved).
    if (++soloScanDivider_ >= 8) { soloScanDivider_ = 0; soloFabricScan(); }
    if (++heartbeatDivider_ >= 30)
    {
        heartbeatDivider_ = 0;
        // Bump heartbeat so the consumer can detect we're alive vs. crashed.
        // Registered-but-inactive Links heartbeat too — they must stay
        // visible (not reaped as stale) so remote re-activation works.
        if (rackLeaseActive_)
            EchoJay_NSLog(("EJCtx(link): leased muteWant="
                + juce::String(rackLeaseMuteWant_.load(std::memory_order_relaxed)
                                   ? "Y" : "N")).toRawUTF8());
        if (regSlotIdx >= 0)
        {
            LinkShm::bumpHeartbeat(regMap, regSlotIdx);
            // Mirror current heartbeat into diag for the editor to read
            if (regMap)
                diag.heartbeat = LinkShm::loadRelaxed(
                    &LinkShm::regSlots(regMap)[regSlotIdx].heartbeat);
        }
        else
        {
            // THE CLAIM RETRY (26 Aug 2026 regression): updateShmState is
            // EVENT-driven — nothing recalls it after the UidClaimGate's
            // Wait arm returns unclaimed, so a Link probing its own ghost
            // waited FOREVER and no Link registered at all. While
            // unregistered, retry the claim here, once per second — the
            // cadence every Wait/adopt threshold was designed against.
            claimRegistrySlot();
        }
    }

#if ECHOJAY_LINK_STATE_DIAG
    // Item-1 diagnosis: log the Active value once after full initialisation
    if (!loggedInitState_)
    {
        loggedInitState_ = true;
        EchoJay_NSLog(("EJLinkState: post-init linkOn="
                       + juce::String((int)linkOn.load())
                       + " name=\"" + linkName + "\" uid=" + instanceUid_).toRawUTF8());
    }
#endif

    // Phase N: adopt a freshly arrived host track name. updateShmState is
    // the established rename path (re-claim slot, republish displayName);
    // repeated host renames coalesce through the dirty flag, and a restore
    // that seeded the same name is a no-op via appliedHostName_.
    if (hostNameDirty_.exchange(false, std::memory_order_acq_rel))
    {
        auto n = getHostTrackName();
        if (n != appliedHostName_)
        {
            appliedHostName_ = n;
            EchoJay_NSLog(("EJLinkState: host track name \"" + n
                           + "\" (user name \"" + linkName + "\")").toRawUTF8());
            updateShmState();
        }
    }

    // Polls keep their pre-30Hz cadence (~100ms): every 3rd tick. Only the
    // meter publish wants the full rate.
    if (heartbeatDivider_ % 3 == 0)
    {
        pollChainCommand();
        pollEditLease();
        pollRackLock();
        pollControlCommand();
        pollKeyCommand();
        pollSessionProjectName();
        publishRackSidecar();   // Phase R: revision-gated, usually a no-op
    }
    // Passive key detection duty cycle — once per second is plenty for a
    // scheduler whose shortest interval is 30 s.
    if (heartbeatDivider_ % 30 == 0)
        schedulePassiveKeyPass();
    // Structure-plan journal check (phase 2): AFTER the DAW's session
    // restore has settled — the restore runs at instantiation and this is
    // the first quiet 1s tick after it — and at most once per process (the
    // journal's delete-on-completion makes a rerun read absent anyway).
    if (!planJournalChecked_ && heartbeatDivider_ % 30 == 0
        && !resolvedDir.isEmpty() && instanceUid_.isNotEmpty())
    {
        planJournalChecked_ = true;
        if (chainHost.planJournalRestoreIfPresent(resolvedDir, instanceUid_))
            // The restore rebuilt the rack in chainHost — same four-step
            // sync, or a crash-recovered Link shows the pre-crash shape
            // until its next local edit (the identical defect, latent).
            syncModelAfterStructuralChange();
    }
    publishMeterFrame();

    // One-shot arming note for the position stamps (stage 0), off the audio
    // thread. Doubles as the behavioural marker: a Link on a host that
    // supplies playhead positions logs this exactly once per instance.
    if (!stampArmedLogged_ && stampsArmed_.load(std::memory_order_relaxed))
    {
        stampArmedLogged_ = true;
        EchoJay_NSLog("EJLinkRing: position stamps armed");
    }
}

void LinkProcessor::publishRackSidecar()
{
    if (instanceUid_.isEmpty()) return;
    if (resolvedDir.isEmpty())
    {
        int err = 0;
        resolvedDir = LinkShm::resolveDir(err);
        if (resolvedDir.isEmpty()) return;
    }
    // THE CURVE IS POLLED, NOT NOTIFIED, and that is the whole fix for the
    // bug where it drew once and never moved again.
    //
    // The obvious design is to publish when something tells you a parameter
    // changed. It does not work HERE, because the built-in devices have no
    // parameters to change: SurgicalEqProcessor deliberately runs without an
    // APVTS and without juce parameters (its editor calls setBand and
    // applyStructured straight into the engine), and nothing on that path
    // calls updateHostDisplay either. So audioProcessorParameterChanged and
    // audioProcessorChanged NEVER fire for the EQ, the hosted-change epoch
    // never moves, and a publish gated on notifications is silent forever.
    // The one publish that did happen came from chainRevision when the EQ was
    // ADDED, which is exactly the reported symptom.
    //
    // So the authoritative trigger is the DATA. getMagnitudeResponse is 64
    // points times kMaxBands complex multiplies with no FFT and no allocation
    // (analysisScratch_ is a member), which at this poll rate is nothing, and
    // unlike a notification it cannot be silently defeated by a device that
    // forgets to announce itself. The epoch stays as an ADDITIONAL trigger:
    // it is correct for real hosted plugins and costs nothing here.
    const int  rev      = chainHost.getChainRevision();
    const int  epoch    = chainHost.getHostedChangeEpoch();
    const bool uidMoved = (instanceUid_ != lastPublishedUid_);   // 6 Sep 2026: a re-minted identity moves its sidecar with it, or the solo fabric cannot find it
    const bool revMoved = (rev   != lastPublishedRackRev_);
    const bool epMoved  = (epoch != lastPublishedEpoch_);
    // §8 closed loop: a mute-state FLIP must republish promptly — the main
    // confirms the commanded mute through this sidecar, and a stale
    // muteEngaged would false-trip the watchdog into solo.
    const bool muteNow = linkMuteWanted();
    const bool muteMoved = (muteNow != lastPublishedMuteEngaged_)
        || (muteUserOn_.load(std::memory_order_relaxed) != lastPublishedMuteUser_)
        || (soloOn_.load(std::memory_order_relaxed) != lastPublishedSoloOn_);
    const double nowMs  = juce::Time::getMillisecondCounterHiRes();

    // The EQ curve, fetched ONCE for the rack rather than per slot: the
    // accessor already resolves "the first built-in EQ" and there is at most
    // one curve to publish. Absent (no EQ, or the slot could not answer) it
    // stays empty and NO eqMagDb key is written, which is what makes an old
    // Link and a new one indistinguishable to a reader: both simply have no
    // curve, and neither claims a flat one.
    std::vector<int16_t> curve;
    const int eqSlot = chainHost.findFirstBuiltinEqSlot();
    if (eqSlot >= 0)
    {
        std::vector<int16_t> tmp((size_t) LinkShm::kEqCurvePoints, (int16_t) 0);
        if (chainHost.getBuiltinEqCurveDeciDb(tmp.data(), LinkShm::kEqCurvePoints))
            curve = std::move(tmp);
    }
    // Track when the curve last MOVED, so a drag can coalesce. Compared
    // against the last COMPUTED curve, not the last published one: otherwise
    // every tick of a slow drag would look like "still changing" relative to
    // a published value that is deliberately behind.
    if (curve != lastComputedCurve_)
    {
        lastComputedCurve_ = curve;
        lastCurveChangeMs_ = nowMs;
    }
    const bool curveMoved = (curve != lastPublishedCurve_);

    // Pre-gain moved (value, hand-set flag, or input-known state), polled the
    // same way as the curve because it fires no notification. Publishing on
    // an input-known transition lets a stale "--" on the main plugin resolve
    // to the real figure once audio is heard.
    const float pgDb    = chainHost.getPreGainDb();
    const bool  pgUser  = chainHost.isPreGainUserSet();
    const bool  pgKnown = chainHost.getChainInLevels().known;
    const bool preGainMoved = (pgDb    != lastPublishedPreGainDb_)
                           || (pgUser  != lastPublishedPreGainUserSet_)
                           || (pgKnown != lastPublishedPreGainInputKnown_);

    if (!revMoved && !epMoved && !curveMoved && !preGainMoved && !muteMoved && !uidMoved)
        return;
    if (!revMoved && !preGainMoved)
    {
        // A settle test ALONE would starve under sustained automation: a host
        // automating an EQ emits changes every block, so the "last change"
        // stamp never falls behind and a pure settle would wait forever,
        // leaving the curve frozen exactly while it is moving most. So the
        // settle coalesces a gesture and the staleness bound guarantees
        // progress. Whichever comes first.
        const double lastMoveMs = juce::jmax(chainHost.lastHostedChangeMs(),
                                             lastCurveChangeMs_);
        const bool settled = (nowMs - lastMoveMs)         >= kRackSettleMs;
        const bool stale   = (nowMs - lastRackPublishMs_) >= kRackMaxStaleMs;
        if (!settled && !stale) return;
    }
    lastPublishedUid_ = instanceUid_; lastPublishedRackRev_ = rev;
    lastPublishedMuteEngaged_ = muteNow;
    lastPublishedMuteUser_ = muteUserOn_.load(std::memory_order_relaxed);
    lastPublishedSoloOn_   = soloOn_.load(std::memory_order_relaxed);
    lastPublishedEpoch_   = epoch;
    lastPublishedCurve_   = curve;
    lastPublishedPreGainDb_        = pgDb;
    lastPublishedPreGainUserSet_   = pgUser;
    lastPublishedPreGainInputKnown_ = pgKnown;
    lastRackPublishMs_    = nowMs;

    LinkShm::RackSidecar rc;
    rc.valid     = true;
    rc.uid       = instanceUid_;
    rc.borrowCapable = true;   // this binary honors the rack-scoped lease
    rc.structureEditCapable = true;   // and can journal/apply a structure plan
    rc.inContextCapable     = true;   // §8: mutes on lease muteOut
    {
        // Round 53 (C4): who we are, so a main counts us only from its own host.
        const auto& h = ChainHost::getHostIdentity();
        rc.publisherPid  = (int) ::getpid();
        rc.hostPid       = h.pid;
        rc.hostStartSec  = h.startSec;
        rc.hostStartUsec = h.startUsec;
    }
    rc.muteEngaged = linkMuteWanted();   // ACTUAL silence, any reason
    rc.muteUser = muteUserOn_.load(std::memory_order_relaxed);
    rc.soloOn   = soloOn_.load(std::memory_order_relaxed);
    rc.muteSoloCapable = true;   // this binary composes and scans
    rc.name      = effectiveDisplayName();
    rc.revision  = rev;
    rc.masterWet = chainHost.getMasterWet();
    // Pre-chain gain mirror (18 Aug 2026): the main plugin's mixer shows and
    // drives it in Pre mode. inputKnown so a strip with no level heard shows
    // "unset" rather than a confident 0.
    rc.preGainDb         = pgDb;
    rc.preGainUserSet    = pgUser;
    rc.preGainInputKnown = pgKnown;
    {
        const auto infos = chainHost.getAllSlotInfos();
        for (int i = 0; i < (int) infos.size(); ++i)
        {
            const auto& s = infos[(size_t) i];
            rc.slots.push_back({ s.name, s.format,
                                 s.settings.substring(0, 200),   // bound file size
                                 s.bypassed, s.wet,
                                 // Stage 1: the leased slot says so, so the
                                 // main plugin can see its lease landed (and
                                 // see it die: controlled vanishing while it
                                 // still holds the lease is the teardown
                                 // signal).
                                 leaseActive_.load(std::memory_order_relaxed)
                                     && (rackLeaseActive_ || i == leaseSlot0_),
                                 // The curve rides on the EQ's OWN slot, so a
                                 // reader never has to guess which entry it
                                 // describes. Every other slot leaves it empty.
                                 i == eqSlot ? curve : std::vector<int16_t>{} });
            // Identity so the slot can enter the server's fp union and dial.
            const auto id = chainHost.getSlotIdentity(i);
            auto& back = rc.slots.back();
            back.fp = id.fp; back.uid = id.uid; back.version = id.version;
            // Rack lock recency: the Link's last LOCAL rack edit rides every
            // slot (assigned by name, after the positional init, like the
            // identity trio above).
            back.lastEditMs = lastLocalRackEditMs_;
            // Manufacturer from the backfilled single source (SlotInfo),
            // assigned after the positional init like the trio above.
            back.manufacturer = s.manufacturer;
        }
    }
    LinkShm::writeRackSidecar(resolvedDir, rc);
}

void LinkProcessor::publishMeterFrame()
{
    // Active only — an inactive Link publishes nothing new, so the main
    // plugin's strip freezes and dims within a second (its seq stops moving)
    if (regMap == nullptr || regSlotIdx < 0 || !linkOn.load(std::memory_order_acquire))
        return;
    // Audio liveness: if processBlock hasn't advanced for ~1s, the host has
    // idled this channel — the engine's values are frozen mid-song. Mark the
    // frame audioStale and blank the momentary group (absent convention);
    // integrated/LRA/PLR keep their last-programme values, mirroring how
    // the Meters tab treats stopped audio.
    const uint32_t blocksNow = audioBlockCounter_.load(std::memory_order_relaxed);
    const uint32_t tNowMs    = juce::Time::getMillisecondCounter();
    if (blocksNow != lastSeenBlockCount_)
    {
        lastSeenBlockCount_  = blocksNow;
        lastBlockAdvanceMs_  = tNowMs;
    }
    const bool audioStale = (tNowMs - lastBlockAdvanceMs_) > 1000;
    if (audioStale != audioWasStale_)
    {
        audioWasStale_ = audioStale;
        EchoJay_NSLog(("EJLinkMeter: \"" + (linkName.isEmpty() ? "(untitled)" : linkName)
                       + (audioStale
                          ? "\" audio stale (blocks frozen at " + juce::String(blocksNow)
                            + ") -> momentary group absent"
                          : "\" audio resumed (blocks " + juce::String(blocksNow) + ")")).toRawUTF8());
    }

    auto md = meterEngine_.getMeterData();
    LinkMeterFrame f;   // global-scope struct, same as RegistrySlot
    f.momentary   = md.momentary;
    f.shortTerm   = md.shortTerm;
    f.integrated  = md.integrated;
    f.rmsL        = md.rmsL;
    f.rmsR        = md.rmsR;
    f.peakL       = md.peakL;
    f.peakR       = md.peakR;
    f.truePeakMax = juce::jmax(md.truePeakMaxL, md.truePeakMaxR);
    f.truePeakCur = juce::jmax(md.truePeakL, md.truePeakR);
    f.lra         = md.loudnessRange;
    f.shortTermTP = md.shortTermTruePeak;
    f.peakFastL   = md.peakFastL;
    f.peakFastR   = md.peakFastR;
    f.fieldsMask  = kFrameHasFastPeak;   // this writer populates the fast pair
    f.audioBlocks = blocksNow;
    f.audioStale  = audioStale ? 1u : 0u;
    if (audioStale)
    {
        // Momentary group -> absent convention (dashes); rms/peak persist
        // as last-programme values and the receiver dims them
        f.momentary   = -100.0f;
        f.shortTerm   = -100.0f;
        f.shortTermTP = -100.0f;   // gates PSR to '--' like the Meters tab
        // A FAST meter showing a seconds-old peak as current is fake motion:
        // the fast pair blanks with the momentary group (rms/slow peak
        // persist dimmed, the standing convention).
        f.peakFastL   = -100.0f;
        f.peakFastR   = -100.0f;
    }
    f.crest       = md.crestFactor;
    f.correlation = md.correlation;
    f.width       = md.width;
    // Pink-referenced rels: db - mean(valid bands) — same derivation as the
    // chat JSON; macroBandDb itself holds ABSOLUTE per-octave dB
    float mean = 0.0f; int n = 0;
    for (auto db : md.macroBandDb)
        if (db > -119.0f) { mean += db; ++n; }
    if (n > 0)
    {
        mean /= (float) n;
        for (size_t i = 0; i < 6; ++i)
            f.bandRel[i] = md.macroBandDb[i] > -119.0f ? md.macroBandDb[i] - mean : 0.0f;
    }
    // Detected key (KEY_DETECTOR_SPEC.md §9): the passive duty-cycled reading,
    // published in the appended key group. Gated by kFrameHasKey so an old
    // reader ignores it and a new reader never mistakes an old writer's
    // zeroed pad for C major.
    //
    // When this Link's chain HOSTS an EchoJay Key Detector (the Tier 1 path:
    // the main plugin added it and triggered ANALYSE), the device's reading
    // is preferred — it is the explicit, committed mechanism, and preferring
    // it is what carries a Tier 1 result back to the main plugin without any
    // second transport. The passive reading remains the fallback.
    {
        auto kr = keyEngine_.getReading();
        juce::uint32 stamp = keyWorker_.lastChangeMs();
        for (int i = 0; i < chainHost.getNumSlots(); ++i)
            if (auto* kd = dynamic_cast<EedKeyDetectorProcessor*>(chainHost.getSlotProcessor(i)))
            {
                if (const auto dr = kd->engine().getReading(); dr.valid)
                {
                    kr    = dr;
                    stamp = kd->readingChangeMs();
                }
                break;   // one detector speaks for the chain
            }
        if (kr.valid)
        {
            f.keyRoot       = (int16_t) kr.root;
            f.keyIsMinor    = kr.minor ? 1 : 0;
            f.keyConfidence = kr.confidence;
            f.keyTuningHz   = kr.tuningHz;
            f.keyAgeMs = stamp != 0 ? juce::Time::getMillisecondCounter() - stamp : 0;
        }
        f.fieldsMask |= kFrameHasKey;
    }

    // Frozen-engine guard: when the transport stops, the host stops calling
    // processBlock, the engine's values freeze, and stamping them would keep
    // the strip "fresh" scrolling a constant plateau forever — fake motion.
    // Real audio NEVER produces two byte-identical frames (LUFS jitters), so
    // an identical payload means frozen: skip the publish, seq stops
    // advancing, and the receiver's staleness path freezes+dims the strip.
    //
    // keyAgeMs is EXCLUDED from the comparison (equalised before the memcmp):
    // it advances on every tick by construction, and letting it count as
    // "motion" would defeat this guard entirely — every frame would differ,
    // seq would never stop, and the receiver could no longer tell a frozen
    // engine from a live one. A stalled Link's published key age freezes with
    // the rest of the frame; the receiver's seq-staleness already covers it.
    {
        LinkMeterFrame cmp = f;
        cmp.keyAgeMs = lastPublishedFrame_.keyAgeMs;
        if (std::memcmp(reinterpret_cast<const uint8_t*>(&cmp) + sizeof(uint32_t),
                        reinterpret_cast<const uint8_t*>(&lastPublishedFrame_) + sizeof(uint32_t),
                        sizeof(LinkMeterFrame) - sizeof(uint32_t)) == 0)
            return;
    }
    lastPublishedFrame_ = f;
    LinkShm::publishMeterFrame(regMap, regSlotIdx, f);
    // Frame diagnostics: first 3 frames after (re)activation, then 1 per 10s
    ++meterFramesPublished_;
    if (meterFramesPublished_ <= 3 || meterFramesPublished_ % 100 == 0)
        EchoJay_NSLog(("EJLinkMeter: \"" + (linkName.isEmpty() ? "(untitled)" : linkName)
                       + "\" #" + juce::String(meterFramesPublished_)
                       + " mom=" + juce::String(f.momentary, 1)
                       + " rms=" + juce::String(f.rmsL, 1) + "/" + juce::String(f.rmsR, 1)
                       + " blocks=" + juce::String(f.audioBlocks)
                       + " tp=" + juce::String(f.truePeakMax, 1)
                       + " tpCur=" + juce::String(f.truePeakCur, 1)
                       + " corr=" + juce::String(f.correlation, 2)
                       + " rel=[" + juce::String(f.bandRel[0], 1) + " " + juce::String(f.bandRel[5], 1)
                       + "]").toRawUTF8());
}

void LinkProcessor::pollSessionProjectName()
{
    auto shared = ChainHost::getSessionProjectName();
    if (shared != lastSeenSharedProject_)
    {
        // Adopt when we have no name of our own; follow when our name equals
        // the PREVIOUS shared value (we were following the session). A
        // restored distinct name never gets overwritten.
        if (projectName.trim().isEmpty()
            || projectName.trim() == lastSeenSharedProject_.trim())
        {
            projectName = shared;
            updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
        }
        lastSeenSharedProject_ = shared;
    }

    // Session genre — same rules (Link has no genre default, so empty
    // genuinely means "never had one" here, unlike the main plugin)
    auto sharedG = ChainHost::getSessionGenre();
    if (sharedG != lastSeenSharedGenre_)
    {
        if (genre.trim().isEmpty()
            || genre.trim() == lastSeenSharedGenre_.trim())
        {
            genre = sharedG;
            updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
        }
        lastSeenSharedGenre_ = sharedG;
    }
}

// ---------------------------------------------------------------------------
// Remote Active control — ctrl-cmd-<instanceId>.json {v:1, seq, active}
// Authority stays HERE: the command flips linkOn exactly like the local
// toggle (same updateShmState path, same dirty-marking so it persists),
// then the ack confirms the applied state.
// ---------------------------------------------------------------------------
void LinkProcessor::pollRackLock()
{
    // ~100ms cadence, message thread — same shape as pollEditLease: the FILE
    // is the claim, freshness decides, and the transition is what acts.
    if (instanceUid_.isEmpty() || resolvedDir.isEmpty()) return;
    juce::String id, owner;
    double age = 1.0e12;
    LinkShm::readRackLockFile(resolvedDir, instanceUid_, id, owner, age);
    const auto claim = LinkShm::RackLock::read(id, age, {});   // a Link never owns
    const juce::String newOwner =
        claim == LinkShm::RackLock::Claim::Other
            ? (owner.isNotEmpty() ? owner : juce::String("another EchoJay"))
            : juce::String();
    if (newOwner == rackLockOwner_) return;
    EchoJay_NSLog((newOwner.isNotEmpty()
                       ? "EJRackLock: locked by \"" + newOwner + "\""
                       : "EJRackLock: released (was \"" + rackLockOwner_ + "\")")
                      .toRawUTF8());
    rackLockOwner_ = newOwner;
    notifyChainModel();   // the editor re-renders the rack UI locked/unlocked
}

bool LinkProcessor::rackLockGuard(const char* op)
{
    if (rackLockOwner_.isEmpty()) return false;
    // The refusal asserts itself: from outside, a guard that returned early
    // is indistinguishable from one that refused — this line is the witness.
    EchoJay_NSLog(("EJRackLock: refused " + juce::String(op)
                   + " - rack is selected on \"" + rackLockOwner_ + "\"").toRawUTF8());
    return true;
}

void LinkProcessor::pollEditLease()
{
    // ~100ms cadence, message thread. The FILE is the lease (see LinkShm.h):
    // parse what is there, let the pure LeaseGate decide, act on the verdict.
    if (instanceUid_.isEmpty() || resolvedDir.isEmpty()) return;

    juce::String fileId;
    int          fileSlot1 = 0;
    bool         fileScopeRack = false;
    double       ageMs     = 1.0e12;   // absent reads as infinitely stale
    juce::File f(LinkShm::leasePath(resolvedDir, instanceUid_));
    if (f.existsAsFile())
    {
        auto v = juce::JSON::parse(f.loadFileAsString());
        if (auto* o = v.getDynamicObject())
        {
            fileId    = o->getProperty("leaseId").toString();
            fileSlot1 = (int) o->getProperty("slot");
            fileScopeRack = o->getProperty("scope").toString() == "rack";
            // §8 in-context: the mute rides the LEASE (one lifetime by
            // construction — the expiry that restores bypasses restores
            // the mute; no second restore path can exist). Re-read every
            // poll so LISTEN's solo mode can lift it live.
            rackLeaseMuteWant_.store(o->hasProperty("muteOut")
                && (bool) o->getProperty("muteOut"),
                std::memory_order_relaxed);
            rackLeaseEditPending_.store(o->hasProperty("editPending")
                && (bool) o->getProperty("editPending"),
                std::memory_order_relaxed);
            ageMs     = (double) juce::Time::currentTimeMillis()
                          - (double) (juce::int64) o->getProperty("tMs");
        }
    }
    else
    {
        // No file. If we are not engaged either, this is the overwhelmingly
        // common case and the gate call below is a no-op; skip the work.
        if (!leaseActive_.load(std::memory_order_relaxed)) return;
    }

    switch (leaseGate_.poll(fileId, fileSlot1, ageMs))
    {
        case LinkShm::LeaseGate::Engage:
        {
            // THE SCOPE DECISION, through the shared pure helper (step 2):
            // rack scope on a capable binary engages the whole rack; on an
            // old binary the same file (slot 0, scope unparsed) falls into
            // the slot arm and refuses — proven equivalent in racklock_test.
            const auto scoped = LinkShm::LeaseScope::decide(
                fileScopeRack, /*binarySupportsRack*/ true,
                leaseGate_.activeSlot1, chainHost.getNumSlots());
            if (scoped == LinkShm::LeaseScope::Engage::Rack)
            {
                rackLeaseEngage();
                break;
            }
            const int slot0 = leaseGate_.activeSlot1 - 1;
            if (scoped == LinkShm::LeaseScope::Engage::Refuse
                || slot0 < 0 || slot0 >= chainHost.getNumSlots())
            {
                // A lease naming a slot this rack does not have engages
                // NOTHING: force the gate back to idle so a later valid
                // lease can engage, and remember nothing.
                leaseGate_.activeId.clear(); leaseGate_.activeSlot1 = 0;
                break;
            }
            leaseSlot0_       = slot0;
            leasePriorBypass_ = chainHost.getSlotInfo(slot0).bypassed;
            // Bypass the slot for the lease's duration: the ring taps
            // post-rack, so without this the editing copy would receive
            // audio ALREADY processed by this instance and process it again
            // in series. setSlotBypassed bumps chainRevision, so the sidecar
            // republishes with the controlled flag riding along.
            chainHost.setSlotBypassed(slot0, true);
            leaseActive_.store(true, std::memory_order_relaxed);
            notifyChainModel();   // the open editor dims + disables the slot
            EchoJay_NSLog(("EJLease: engaged slot "
                           + juce::String(slot0 + 1)).toRawUTF8());
            break;
        }
        case LinkShm::LeaseGate::Expire:
        case LinkShm::LeaseGate::Release:
        {
            // ONE restore path for every ending -- clean release, expiry
            // after a crash, a new id superseding a dead session -- and for
            // BOTH scopes: the rack arm restores every slot's saved bypass,
            // the slot arm restores its one, through this same switch case.
            leaseActive_.store(false, std::memory_order_relaxed);
            rackLeaseMuteWant_.store(false, std::memory_order_relaxed);
            rackLeaseEditPending_.store(false, std::memory_order_relaxed);
            if (rackLeaseActive_)
            {
                rackLeaseRelease();
            }
            else if (leaseSlot0_ >= 0 && leaseSlot0_ < chainHost.getNumSlots())
            {
                chainHost.setSlotBypassed(leaseSlot0_, leasePriorBypass_);
                EchoJay_NSLog("EJLease: released/expired - slot restored");
            }
            leaseSlot0_ = -1;
            notifyChainModel();
            break;
        }
        case LinkShm::LeaseGate::Hold:
        case LinkShm::LeaseGate::None:
            break;
    }
}

// ---------------------------------------------------------------------------
// Solo fabric scan (27 Aug 2026): the solo set IS the live sidecars. Walk
// the registry (shared memory, free), keep per-slot liveness, and parse a
// foreign sidecar ONLY when its file's mtime moved. A row counts toward the
// set only while proven live AND its heartbeat moved within ~3.5s —
// RegLiveness::proven is sticky, so death needs its own freshness check.
// This is what makes a deleted or crashed soloed Link self-healing: the
// want is re-derived from live evidence every tick and stored nowhere.
// ---------------------------------------------------------------------------
void LinkProcessor::soloFabricScan()
{
    if (regMap == nullptr) return;
    int err = 0;
    const juce::String dir = LinkShm::resolveDir(err);
    if (dir.isEmpty()) { soloMuteWant_.store(false, std::memory_order_relaxed); return; }
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    bool anyForeignSolo = false;
    auto* slots = LinkShm::regSlots(regMap);
    for (int i = 0; i < kRegMaxSlots; ++i)
    {
        auto& row = soloScan_[(size_t) i];
        if (LinkShm::loadAcquire(&slots[i].inUse) == 0) { row = {}; continue; }
        char ub[13] = {};
        std::memcpy(ub, slots[i].instanceUid, 12);
        const juce::String uid = juce::String::fromUTF8(ub);
        if (uid.isEmpty() || uid == instanceUid_) { row = {}; continue; }
        if (row.uid != uid) { row = {}; row.uid = uid; row.lastHbMoveMs = nowMs; }
        const uint32_t hb = LinkShm::loadRelaxed(&slots[i].heartbeat);
        if (hb != row.lastHb) { row.lastHb = hb; row.lastHbMoveMs = nowMs; }
        const bool proven = row.live.observe(hb);
        const bool fresh  = (nowMs - row.lastHbMoveMs) < 3500.0;
        if (! proven || ! fresh) continue;    // never muted by a ghost
        juce::File f(LinkShm::rackSidecarPath(dir, uid));
        const juce::int64 mt = f.existsAsFile()
            ? f.getLastModificationTime().toMilliseconds() : 0;
        if (mt != row.mtimeMs)
        {
            row.mtimeMs = mt;
            const auto rc = LinkShm::readRackSidecar(dir, uid);
            row.soloOn = (rc.uid == uid) && rc.soloOn;
        }
        anyForeignSolo = anyForeignSolo || row.soloOn;
    }
    // In the set = exempt: a soloed Link never solo-mutes itself, and
    // multi-solo means everyone in the set plays.
    soloMuteWant_.store(anyForeignSolo
                            && ! soloOn_.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
}

void LinkProcessor::pollControlCommand()
{
    auto id = chainInstanceId();
    if (id.isEmpty()) return;
    if (resolvedDir.isEmpty())
    {
        int err = 0;
        resolvedDir = LinkShm::resolveDir(err);
        if (resolvedDir.isEmpty()) return;
    }

    juce::File cmdFile(resolvedDir + "ctrl-cmd-" + id + ".json");
    if (!cmdFile.existsAsFile()) return;

    // CONSUME AND ANSWER, ALWAYS (24 Aug 2026 ruling): a command is
    // consumed exactly once and always answered — ver mismatch, duplicate
    // seq, malformed, all of them. Silence is never a valid response; a
    // refused-but-kept file wedged this channel at 10Hz and left the
    // sender's poll blind until its timeout.
    auto refuseCmd = [&](const juce::String& why, int rseq)
    {
        cmdFile.deleteFile();
        auto* r = new juce::DynamicObject();
        r->setProperty("v",       1);
        r->setProperty("seq",     rseq);
        r->setProperty("refused", why);
        juce::File(resolvedDir + "ctrl-ack-" + id + ".json")
            .replaceWithText(juce::JSON::toString(juce::var(r), true));
        EchoJay_NSLog(("EJCtrl: link REFUSED ctrl-cmd (" + why + ") seq="
            + juce::String(rseq)
            + " lastApplied=" + juce::String(lastAppliedCtrlSeq_)
            + " - consumed and answered").toRawUTF8());
    };

    auto v = juce::JSON::parse(cmdFile.loadFileAsString());
    auto* obj = v.getDynamicObject();
    if (obj == nullptr) { refuseCmd("UNPARSEABLE ctrl-cmd", 0); return; }

    int ver = (int)obj->getProperty("v");
    int seq = (int)obj->getProperty("seq");
    if (ver != 1)
    { refuseCmd("version " + juce::String(ver) + " unsupported", seq); return; }
    if (seq == 0)
    { refuseCmd("seq 0 invalid", seq); return; }
    if (seq == lastAppliedCtrlSeq_)
    { refuseCmd("duplicate seq, already applied", seq); return; }

    lastAppliedCtrlSeq_ = seq;
    cmdFile.deleteFile();   // consumed

    // Active is applied ONLY when present. The main plugin always includes
    // the current active in gain/match commands, so this is normally a
    // no-op there; guarding on presence means a hand-built gain-only command
    // can never spuriously deactivate the Link. (v stays 1: the fields are
    // additive; a pre-gain Link ignores gainDb and reads the echoed active.)
    if (obj->hasProperty("active"))
    {
        bool wantActive = (bool)obj->getProperty("active");
        if (wantActive != linkOn.load())
            EchoJay_NSLog(("EJLinkState: remote set active=" + juce::String((int)wantActive)
                           + " (seq " + juce::String(seq) + ")").toRawUTF8());
        linkOn.store(wantActive);
        updateShmState();                   // registry flag + ring + dirty-mark
        if (onLinkStateChanged) onLinkStateChanged();   // open editor toggle sync
    }

    // Absolute gain set (from the monitor steppers OR the AI level-match
    // action, which the main plugin resolves to an absolute dB before
    // sending). Authority stays here: setGainDb clamps, mirrors to the slot,
    // dirty-marks, and syncs an open editor's slider.
    if (obj->hasProperty("gainDb"))
    {
        float g = (float)(double)obj->getProperty("gainDb");
        EchoJay_NSLog(("EJLinkState: remote set gain=" + juce::String(g, 2)
                       + " dB (seq " + juce::String(seq) + ")").toRawUTF8());
        setGainDb(g);   // snapSmoothing=false → glide (no zipper, no pop)
    }

    // Remote placement declaration (from the monitor row's placement control)
    if (obj->hasProperty("placement"))
        setPlacement((int)obj->getProperty("placement"));

    // Remote pre-chain gain (18 Aug 2026, from the mixer's Pre mode). A fader
    // move is a HAND set: userSet true so the next model build will not
    // overwrite it. Additive field; an old Link never sees it. updateShmState
    // republishes the sidecar (with the new value) and dirty-marks for save.
    if (obj->hasProperty("preGainDb"))
    {
        const float g = (float)(double)obj->getProperty("preGainDb");
        const bool userSet = ! obj->hasProperty("preGainUserSet")
                             || (bool)obj->getProperty("preGainUserSet");
        EchoJay_NSLog(("EJLinkState: remote set pre-gain=" + juce::String(g, 2)
                       + " dB userSet=" + juce::String((int)userSet)
                       + " (seq " + juce::String(seq) + ")").toRawUTF8());
        chainHost.setPreGainDb(g, userSet);
        updateShmState();
    }
    // Mute/solo layer (27 Aug 2026), additive fields on the same cmd
    // transport. muteUser is a MIX decision: dirty-marks via
    // updateShmState so the host saves it. soloOn is monitoring state:
    // applied and published (the fabric reads the sidecar), never saved.
    if (obj->hasProperty("muteUser"))
    {
        const bool on = (bool) obj->getProperty("muteUser");
        EchoJay_NSLog(("EJLinkState: remote set muteUser="
                       + juce::String((int) on)
                       + " (seq " + juce::String(seq) + ")").toRawUTF8());
        muteUserOn_.store(on, std::memory_order_relaxed);
        updateShmState();
        if (onLinkStateChanged) onLinkStateChanged();
    }
    if (obj->hasProperty("soloOn"))
    {
        const bool on = (bool) obj->getProperty("soloOn");
        EchoJay_NSLog(("EJLinkState: remote set soloOn="
                       + juce::String((int) on)
                       + " (seq " + juce::String(seq) + ")").toRawUTF8());
        soloOn_.store(on, std::memory_order_relaxed);
        // My own membership changes my want NOW, not at the next scan:
        // soloing must never briefly mute the soloed strip itself.
        if (on) soloMuteWant_.store(false, std::memory_order_relaxed);
        updateShmState();
    }
    // Reset the pre-gain to auto (clears the hand-set flag; the next build
    // sets it again). Its own field: setPreGainDb userSet=false cannot clear.
    if (obj->hasProperty("preGainReset") && (bool)obj->getProperty("preGainReset"))
    {
        EchoJay_NSLog(("EJLinkState: remote reset pre-gain to auto (seq "
                       + juce::String(seq) + ")").toRawUTF8());
        chainHost.resetPreGainToAuto();
        updateShmState();
    }

    // ---- Remote editor open (stage 1) -------------------------------------
    // 1-BASED like every other slot reference on the wire (slot, to, after);
    // the conversion to a 0-based rack index happens here, once.
    //
    // The instance opened is THIS Link's own, in the signal path, so the user
    // hears their edits immediately. Nothing is transferred and the audio is
    // untouched: this moves a window to the front, nothing more.
    // ---- Slot state pull (stage 1 remote editing) --------------------------
    // The pull IS the size measurement: over the cap or a throwing plugin
    // fails HERE, in the ack, before the main plugin engages any lease,
    // bypass or editing instance. Nothing to unwind on refusal.
    bool pullAttempted = false;
    juce::String pulledB64, pullErr;
    if (obj->hasProperty("pullSlotState"))
    {
        pullAttempted = true;
        const int slot0 = (int) obj->getProperty("pullSlotState") - 1;
        auto* proc = chainHost.getSlotProcessor(slot0);
        if (proc == nullptr)
            pullErr = "slot " + juce::String(slot0 + 1) + " has no plugin";
        else
        {
            juce::MemoryBlock mb;
            bool threw = false;
            try { proc->getStateInformation(mb); } catch (...) { threw = true; }
            if (threw)
                pullErr = "this plugin refused to hand over its settings";
            else if ((int) mb.getSize() > LinkShm::kLinkTransferMaxSlotBytes)
                pullErr = "this plugin's settings are "
                        + juce::File::descriptionOfSizeInBytes((juce::int64) mb.getSize())
                        + ", over the "
                        + juce::File::descriptionOfSizeInBytes((juce::int64) LinkShm::kLinkTransferMaxSlotBytes)
                        + " limit, too large to carry";
            else
                pulledB64 = LinkShm::stateToB64(mb);
            // PHASE 1 BYTE ACCOUNTING (instrumentation, correlation id = seq).
            // Points 1 and 2 of four: raw size out of getStateInformation,
            // and bytes handed to the transport after encoding. The transport
            // is ONE ctrl-ack JSON file, NOT chunked, no framing beyond JSON
            // string quoting; its only cap is the raw-size gate above
            // (LinkShm::kLinkTransferMaxSlotBytes, enforced BEFORE encoding).
            EchoJay_NSLog(("EJPull[" + juce::String(seq) + "] link: raw="
                           + juce::String((juce::int64) mb.getSize())
                           + " bytes from getStateInformation; encoded="
                           + juce::String(pulledB64.length())
                           + " b64 chars; cap="
                           + juce::String(LinkShm::kLinkTransferMaxSlotBytes)
                           + " raw bytes; transport=single ctrl-ack JSON, unchunked"
                           + (pullErr.isNotEmpty() ? "; REFUSED: " + pullErr
                                                   : juce::String())).toRawUTF8());
        }
    }

    // ---- Structure plan (phase 2): verify, journal, two-phase apply --------
    bool planAttempted = false;
    ChainHost::PlanResult planResult;
    if (obj->hasProperty("structPlan"))
    {
        // RECEIPT, logged BEFORE the parse and the apply — so a run's log
        // says which side dropped a plan: no "send" line = main never sent;
        // send but no "received" = transport or this dispatcher; received
        // but no "applied" = the apply hung or died.
        EchoJay_NSLog(("EJPlan[" + juce::String(seq)
                       + "] link: plan received").toRawUTF8());
        planAttempted = true;
        LinkShm::StructureEdit::Plan plan;
        LinkShm::StructureEdit::PreImages ignored;
        if (LinkShm::StructureEdit::planFromVar(obj->getProperty("structPlan"),
                                                plan, ignored))
            // Apply + the four-step sync, one author — notify alone told
            // the editor to re-read a model the plan never wrote (the
            // two-models defect; functionally gated in linksync_test).
            planResult = applyStructurePlanAndSync(resolvedDir, plan);
        else
            planResult.reasons.add("the plan could not be read");
        EchoJay_NSLog(("EJPlan[" + juce::String(seq) + "] link: applied="
                       + juce::String(planResult.ok ? "Y" : "N")
                       + " restored=" + juce::String(planResult.restored ? "Y" : "N")
                       + (planResult.failedAt.isNotEmpty()
                              ? " failedAt=" + planResult.failedAt : juce::String()))
                          .toRawUTF8());
    }

    // ---- Commit (stage 1): the edited state comes home ---------------------
    // Guarded by baseSlots exactly like structural edits: the rack the main
    // plugin was looking at must still be THIS rack, or the state could land
    // on the wrong plugin. Stale = refused with a reason, never applied.
    bool commitAttempted = false, commitOk = false;
    juce::String commitErr;
    if (obj->hasProperty("commitSlot"))
    {
        commitAttempted = true;
        const int slot0 = (int) obj->getProperty("commitSlot") - 1;
        juce::StringArray base;
        if (auto* bs = obj->getProperty("baseSlots").getArray())
            for (auto& bv : *bs) base.add(bv.toString().trim());
        const auto infos = chainHost.getAllSlotInfos();
        bool stale = ((int) infos.size() != base.size());
        if (!stale)
            for (int i = 0; i < (int) infos.size(); ++i)
                if (infos[(size_t) i].name.trim() != base[i]) { stale = true; break; }
        if (stale)
            commitErr = "the rack changed while you were editing - nothing was applied";
        else if (auto* proc = chainHost.getSlotProcessor(slot0))
        {
            juce::MemoryBlock mb;
            const juce::String cIn = obj->getProperty("commitState").toString();
            const bool cOk = LinkShm::stateFromB64(cIn, mb);
            if (!cOk || mb.getSize() == 0)
                EchoJay_NSLog(("EJCommit[" + juce::String(seq)
                    + "] link: decode FAILED (Base64::convertFromBase64, in="
                    + juce::String(cIn.length()) + " b64 chars, out="
                    + juce::String((juce::int64) mb.getSize())
                    + " bytes)").toRawUTF8());
            if (cOk && mb.getSize() > 0)
            {
                try
                {
                    proc->setStateInformation(mb.getData(), (int) mb.getSize());
                    commitOk = true;
                }
                catch (...) { commitErr = "this plugin refused the settings"; }
            }
            else commitErr = "settings did not survive the trip (decode failed)";
        }
        else commitErr = "slot " + juce::String(slot0 + 1) + " has no plugin";
    }

    bool openAttempted = false, openSucceeded = false;
    if (obj->hasProperty("openSlot"))
    {
        openAttempted = true;
        const int slot0 = (int)obj->getProperty("openSlot") - 1;
        // A null callback means this Link has no editor open, and only a
        // window can raise a window. That is a boundary, not a failure, and
        // the ack says so rather than the main plugin guessing from silence.
        if (onOpenSlotEditor && slot0 >= 0)
            openSucceeded = onOpenSlotEditor(slot0);
    }

    auto* ack = new juce::DynamicObject();
    ack->setProperty("v",         1);
    ack->setProperty("seq",       seq);
    ack->setProperty("active",    linkOn.load());
    ack->setProperty("gainDb",    (double)gainDb_.load(std::memory_order_relaxed));
    ack->setProperty("placement", placement_.load(std::memory_order_relaxed));
    // CROSS-VERSION: this key is what tells an "opened" apart from a Link too
    // old to know the field at all. An older build ignores openSlot and still
    // writes a perfectly normal ack, so its ABSENCE is the version signal.
    // Written only when the command actually asked, so ordinary Active and
    // gain acks stay byte-identical to what old readers expect.
    if (openAttempted)
        ack->setProperty("openedSlot", openSucceeded);
    // Same absence-is-the-version-signal contract as openedSlot: these keys
    // exist only when the command asked, so an old Link's ack simply lacks
    // them and the main plugin reports "too old" instead of claiming success.
    if (pullAttempted)
    {
        ack->setProperty("pulledState", pullErr.isEmpty());
        if (pullErr.isEmpty()) ack->setProperty("slotState", pulledB64);
        else                   ack->setProperty("slotStateErr", pullErr);
    }
    if (commitAttempted)
    {
        ack->setProperty("committedSlot", commitOk);
        if (!commitOk) ack->setProperty("commitErr", commitErr);
    }
    if (planAttempted)
    {
        ack->setProperty("planApplied",  planResult.ok);
        ack->setProperty("planRestored", planResult.restored);
        if (planResult.failedAt.isNotEmpty())
            ack->setProperty("planFailedAt", planResult.failedAt);
        juce::Array<juce::var> rs;
        for (const auto& why : planResult.reasons) rs.add(why);
        ack->setProperty("planReasons", rs);
    }
    {
        juce::File af(resolvedDir + "ctrl-ack-" + id + ".json");
        af.replaceWithText(juce::JSON::toString(juce::var(ack), true));
        if (pullAttempted)
            EchoJay_NSLog(("EJPull[" + juce::String(seq) + "] link: ack file "
                           + juce::String((juce::int64) af.getSize())
                           + " bytes on disk").toRawUTF8());
    }
}

// ---------------------------------------------------------------------------
// Passive key detection (KEY_PRECONDITION_SPEC.md §5.1) — the duty cycle.
// Message thread, ~1 Hz. Arms a committed pass when one is due and the gates
// pass; the actual analysis runs on the key worker thread.
// ---------------------------------------------------------------------------
void LinkProcessor::schedulePassiveKeyPass()
{
    const uint32_t now     = juce::Time::getMillisecondCounter();
    const bool     playing = transportPlaying_.load(std::memory_order_relaxed);

    // Invalidation events (§5.4) make the next pass due immediately —
    // computed every tick so a jump during a pass is not lost.
    bool invalidated = false;
    if (keySectionJump_.exchange(false, std::memory_order_relaxed))
        invalidated = true;
    if (playing && ! keyWasPlaying_
        && lastPlayingMs_ != 0 && now - lastPlayingMs_ > kKeyLongGapMs)
        invalidated = true;                        // came back after a long stop
    if (playing) lastPlayingMs_ = now;
    keyWasPlaying_ = playing;
    if (invalidated) lastKeyPassArmMs_ = 0;        // due now (still gated below)

    if (keyEngine_.isCollecting())
    {
        // A pass whose transport stopped under it would eventually fill its
        // window with a silent tail (or a different section) — cancel after
        // a few seconds stopped and let the cycle re-arm on the next play.
        // The engine keeps the previous reading; nothing is lost.
        if (! playing)
        {
            if (++keyStallTicks_ >= 5)
            {
                keyStallTicks_ = 0;
                keyEngine_.cancelAnalysis();
                EchoJay_NSLog("EJLinkKey: pass cancelled (transport stopped)");
            }
        }
        else keyStallTicks_ = 0;
        return;
    }
    keyStallTicks_ = 0;

    // The gates: Active (the tap is only fed while Active), rolling, signal.
    if (! linkOn.load(std::memory_order_acquire)) return;
    if (! playing) return;
    if (meterEngine_.getMeterData().momentary < kKeySignalFloorLufs) return;

    const bool due = lastKeyPassArmMs_ == 0
                  || now - lastKeyPassArmMs_ >= kKeyPassIntervalMs;
    if (! due) return;

    lastKeyPassArmMs_ = now;
    keyEngine_.startAnalysis();
    keyWorker_.notify();
    EchoJay_NSLog(("EJLinkKey: passive pass armed (" + juce::String(kKeyPassWindowS, 0)
                   + " s window)").toRawUTF8());
}

// ---------------------------------------------------------------------------
// Remote RE-ANALYSE — key-cmd-<instanceId>.json {v:1, seq}. The Meters tab's
// RE-ANALYSE button, when its key source is this Link's passive reading.
// Same transport conventions as ctrl-cmd: seq-gated, deleted on consume,
// acked. The pass is armed unconditionally (the engine's own waiting-for-
// signal state stays honest if nothing is playing yet).
// ---------------------------------------------------------------------------
void LinkProcessor::pollKeyCommand()
{
    auto id = chainInstanceId();
    if (id.isEmpty()) return;
    if (resolvedDir.isEmpty())
    {
        int err = 0;
        resolvedDir = LinkShm::resolveDir(err);
        if (resolvedDir.isEmpty()) return;
    }

    juce::File cmdFile(resolvedDir + "key-cmd-" + id + ".json");
    if (!cmdFile.existsAsFile()) return;

    auto v = juce::JSON::parse(cmdFile.loadFileAsString());
    auto* obj = v.getDynamicObject();
    if (obj == nullptr) { cmdFile.deleteFile(); return; }

    int ver = (int)obj->getProperty("v");
    int seq = (int)obj->getProperty("seq");
    if (ver != 1 || seq == lastAppliedKeySeq_ || seq == 0)
        return;

    lastAppliedKeySeq_ = seq;
    cmdFile.deleteFile();   // consumed

    lastKeyPassArmMs_ = juce::Time::getMillisecondCounter();
    keyEngine_.startAnalysis();
    keyWorker_.notify();
    EchoJay_NSLog(("EJLinkKey: remote RE-ANALYSE (seq " + juce::String(seq)
                   + ")").toRawUTF8());

    auto* ack = new juce::DynamicObject();
    ack->setProperty("v",      1);
    ack->setProperty("seq",    seq);
    ack->setProperty("status", "armed");
    juce::File(resolvedDir + "key-ack-" + id + ".json")
        .replaceWithText(juce::JSON::toString(juce::var(ack), true));
}

// =============================================================================
//  Registry helpers (message thread)
// =============================================================================
void LinkProcessor::ensureRegistryOpen()
{
    if (regMap) return;

    // Resolve directory once — persist for ring opens
    if (resolvedDir.isEmpty())
    {
        int dirErr = 0;
        resolvedDir = LinkShm::resolveDir(dirErr);
        if (resolvedDir.isEmpty())
        {
            diag.regKey    = "(no writable dir)";
            diag.regOpened = false;
            diag.regErrno  = dirErr;
            return;
        }
    }

    diag.regKey = resolvedDir;
    int err = 0;
    regMap = LinkShm::openRegistry(resolvedDir, regFd, err);
    diag.regOpened = (regMap != nullptr);
    diag.regErrno  = err;
    if (regMap == nullptr && err == EPROTO)
        EchoJay_NSLog(("EJLinkState: " + lastRegistryLayoutError()).toRawUTF8());
}

juce::String LinkProcessor::effectiveFilePart() const
{
    // UID-KEYED (6 Sep 2026, shoot-day defect): the ring file used to be
    // named from the user-typed linkName, so two Links carrying the same
    // typed name (a duplicated track or a pasted insert restores the same
    // name into both) opened the SAME ring file - openRingProducer zeroes the
    // header on open, so the second instance wiped the first's ring and both
    // rows in the main plugin read one file. Rows were already uid-keyed and
    // distinct (the uid claim gate re-mints a duplicate); only the FILE was
    // keyed on the name. instanceUid_ is unique per instance, saved and
    // restored with the state, re-minted for a proven-live duplicate before
    // this is consulted (claimRegistrySlot), and never changes on a host or
    // user rename - so the ring never renames either. The name stays purely
    // cosmetic: displayName in the registry slot.
    return instanceUid_;
}

void LinkProcessor::updateTrackProperties(const TrackProperties& props)
{
    // ANY thread (AU property listener fires off the message thread), any
    // number of times, fields optional (AU/AAX send name only; a VST3
    // colour-only update carries no name). No shm or host calls from here —
    // stash + flag, timerCallback applies on the message thread.
    if (!props.name.has_value())
        return;   // colour-only update: colour is deferred (Phase N scope)
    const juce::String n = juce::String(*props.name).trim();
    {
        const juce::ScopedLock sl(hostNameLock_);
        hostNameFromHost_ = true;   // AUTHORITATIVE from here on, even if equal to a seeded value
        if (hostTrackName_ == n) return;
        hostTrackName_ = n;
    }
    hostNameDirty_.store(true, std::memory_order_release);
    // PROVENANCE LOG (6 Sep 2026): the only line that proves the HOST delivered
    // a name; the timer's "host track name" line also fires for a seeded one.
    EchoJay_NSLog(("EJLinkState: host DELIVERED track name \"" + n + "\" (uid " + instanceUid_ + ")").toRawUTF8());
}

juce::String LinkProcessor::getHostTrackName() const
{
    const juce::ScopedLock sl(hostNameLock_);
    return hostTrackName_;
}

juce::String LinkProcessor::effectiveDisplayName() const
{
    auto user = linkName.trim();
    if (user.isNotEmpty()) return user;   // user-typed always wins
    return getHostTrackName().trim();     // "" -> main plugin shows Untitled N
}

void LinkProcessor::claimRegistrySlot()
{
    ensureRegistryOpen();
    if (!regMap || regSlotIdx >= 0) return;

    // Duplication guard, HEARTBEAT-DECIDED (25 Aug 2026 ruling): another
    // slot carrying our saved uid is one of three things, and inUse alone
    // cannot tell them apart — the old inUse-only re-mint burned an
    // identity against every GHOST (unclean kill -> frozen slot), which was
    // the uid-churn engine behind the orphaned files and journals. Now:
    // proven-live holder -> WE re-mint (real duplicate, first-alive keeps
    // the uid); observed-frozen holder -> a ghost, reap it and ADOPT our
    // own uid back; undecided -> WAIT unclaimed (the ~1s updateShmState
    // tick retries) — an unproven holder is never adopted.
    {
        RegistrySlot* slots = LinkShm::regSlots(regMap);   // global-scope struct
        int holder = -1;
        uint32_t holderHb = 0;
        for (int i = 0; i < kRegMaxSlots; ++i)
            if (LinkShm::loadAcquire(&slots[i].inUse) != 0
                && instanceUid_ == juce::String::fromUTF8(slots[i].instanceUid))
            { holder = i; holderHb = LinkShm::loadRelaxed(&slots[i].heartbeat); break; }
        if (holder >= 0)
        {
            if (uidGateHolder_ != holder) { uidGate_ = {}; uidGateHolder_ = holder; }
            // C4b (6 Sep 2026): the holder's sidecar names the process that
            // published it (round 53). A dead publisher is adopted at once;
            // an unpublished or unreadable sidecar fails CLOSED (treated as
            // alive, so the time floor applies).
            bool publisherAlive = true;
            {
                const auto rc = LinkShm::readRackSidecar(resolvedDir, instanceUid_);
                if (rc.valid && rc.uid == instanceUid_ && rc.publisherPid > 0)
                    publisherAlive = (::kill((pid_t) rc.publisherPid, 0) == 0);
            }
            switch (uidGate_.observe(holderHb, publisherAlive, juce::Time::currentTimeMillis()))
            {
                case LinkShm::UidClaimGate::Decision::Wait:
                    return;   // undecided: stay unregistered, retry next tick
                case LinkShm::UidClaimGate::Decision::Remint:
                {
                    auto old = instanceUid_;
                    instanceUid_ = juce::String::toHexString(
                        juce::Random::getSystemRandom().nextInt64()).removeCharacters("-").substring(0, 10);
                    // THE CHUNK WAS NOT OURS (6 Sep 2026 ruling, C1 + C2). A
                    // PROVEN-LIVE holder means the state this instance restored
                    // belongs to ANOTHER LIVE instance: Pro Tools seeds a fresh
                    // insert with the plugin's last chunk (observed 16:59:50,
                    // "setState ... post-init uid=<the first Link's> ... host
                    // track name 'bass 2'"). No field in that chunk that answers
                    // "which Link is this" is ours: the uid (re-minted above),
                    // the host track name (C1) and the typed name (C2). Cleared
                    // here, BEFORE claimSlot reads effectiveDisplayName(), so
                    // the row publishes empty and the main plugin numbers it
                    // "Untitled N" (unique by construction: getLinkDisplayList
                    // numbers untitled rows in uid order) until the host's own
                    // TrackNameChanged or the user's typing names this track.
                    // The AdoptGhost arm and a plain restore keep the seeded
                    // names: those are the cases the seeding was built for (a
                    // replacement incarnation on the same track; a session
                    // reopen in a host that never re-sends the name).
                    // the seeded names are dropped by the invariant below (uid changed)
                    EchoJay_NSLog(("EJLinkState: uid " + old + " held by a "
                        "PROVEN-LIVE instance (duplicate) -> regenerated "
                        + instanceUid_).toRawUTF8());
                    break;
                }
                case LinkShm::UidClaimGate::Decision::AdoptGhost:
                    LinkShm::reapSlot(regMap, holder);
                    EchoJay_NSLog(("EJLinkState: uid " + instanceUid_
                        + " held by a FROZEN ghost slot " + juce::String(holder)
                        + (publisherAlive ? " (publisher alive, floor elapsed)" : " (publisher pid DEAD)")
                        + " -> ghost reaped, uid adopted").toRawUTF8());
                    break;
            }
            uidGateHolder_ = -1;
        }
        else
        {
            uidGateHolder_ = -1;
            // NO HOLDER (6 Sep 2026, L5): a chunk this host process run authored,
            // whose uid no slot holds, is a seed from an instance that has since
            // gone - not a reopen from disk. Re-mint; the invariant below drops
            // its names.
            if (chunkAuthoredHere_ && chunkUid_.isNotEmpty() && instanceUid_ == chunkUid_)
            {
                instanceUid_ = juce::String::toHexString(
                    juce::Random::getSystemRandom().nextInt64()).removeCharacters("-").substring(0, 10);
                EchoJay_NSLog(("EJLinkState: uid " + chunkUid_ + " came from a chunk authored in THIS host run "
                    "and no slot holds it (a seed from a gone instance) -> regenerated " + instanceUid_).toRawUTF8());
            }
        }
    }
    // THE INVARIANT (6 Sep 2026 ruling): the seeded names survive only when this
    // instance continues the chunk's identity. Whatever arm ran above - re-mint
    // against a live holder, re-mint with no holder, or any arm nobody has
    // thought of - if the uid this instance ends up with is NOT the uid that
    // arrived in the chunk, no field from that chunk answers "which Link is
    // this": drop the host track name and the typed name. AdoptGhost and a plain
    // restore keep the uid, so they keep the names. Checked BEFORE claimSlot
    // reads effectiveDisplayName(), so the row publishes what is true.
    if (chunkUid_.isNotEmpty() && instanceUid_ != chunkUid_)
    {
        // PROVENANCE (6 Sep 2026 ruling): only a PROVISIONAL (seeded) name is
        // dropped. A name the host delivered or the user typed is authoritative
        // and survives, whether it arrived before or after this point.
        juce::String droppedTyped, droppedHost, keptHost;
        {
            const juce::ScopedLock sl(hostNameLock_);
            if (hostNameFromHost_) keptHost = hostTrackName_;
            else { droppedHost = hostTrackName_; hostTrackName_.clear(); appliedHostName_.clear(); }
        }
        if (typedNameFromUser_) { /* keep */ } else { droppedTyped = linkName; linkName.clear(); }
        EchoJay_NSLog(("EJLinkState: chunk uid " + chunkUid_ + " != this instance " + instanceUid_
            + ": seeded names dropped (typed \"" + droppedTyped + "\", host \"" + droppedHost
            + "\"); authoritative kept (host \"" + keptHost + "\", typed " + (typedNameFromUser_ ? "\"" + linkName + "\"" : juce::String("none")) + ")").toRawUTF8());
    }
    // From here on this instance IS its identity: no chunk is pending. Both
    // fields are cleared, because updateShmState releases and re-claims the
    // slot on every publish (a rename, a host-name arrival) and a re-claim
    // that still saw "authored here, uid == chunk uid" would re-mint AGAIN -
    // seen as three re-mints in a row in the first run of L5.
    chunkUid_.clear();
    chunkAuthoredHere_ = false;
    const juce::String audioFilename = "audio_" + effectiveFilePart() + ".bin";

    regSlotIdx = LinkShm::claimSlot(regMap,
                                     effectiveDisplayName(),
                                     audioFilename,
                                     instanceUid_,
                                     (float)hostSampleRate,
                                     (uint32_t)hostNumChannels);
    if (regSlotIdx < 0)
    {
        // FULL (6 Sep 2026 ruling): reclaim rows whose publisher process is
        // gone, retry once, and if still full SAY SO - to the log and to the
        // user (diag.regFull, shown by the Link editor). Never silent.
        const int reaped = LinkShm::reapDeadPublisherSlots(regMap, resolvedDir);
        if (reaped > 0)
            regSlotIdx = LinkShm::claimSlot(regMap, effectiveDisplayName(), audioFilename, instanceUid_,
                                            (float)hostSampleRate, (uint32_t)hostNumChannels);
        EchoJay_NSLog(("EJLinkState: registry had no free slot; reaped " + juce::String(reaped)
            + " dead-publisher row(s); " + (regSlotIdx >= 0 ? "claimed slot " + juce::String(regSlotIdx)
            : juce::String("STILL FULL - ") + juce::String(kRegMaxSlots) + " slots all held by live publishers; this Link is NOT registered")).toRawUTF8());
    }
    diag.regFull = (regSlotIdx < 0);
    diag.slotIdx = regSlotIdx;
}

void LinkProcessor::releaseRegistrySlot()
{
    if (regSlotIdx >= 0)
    {
        LinkShm::releaseSlot(regMap, regSlotIdx);
        regSlotIdx = -1;
        diag.slotIdx = -1;
    }
}

// =============================================================================
//  Audio ring helpers (message thread manages lifecycle)
// =============================================================================
void LinkProcessor::openRing()
{
    if (resolvedDir.isEmpty()) return;
    const juce::String filename = "audio_" + effectiveFilePart() + ".bin";

    int fd = -1, err = 0;
    void* map = LinkShm::openRingProducer(resolvedDir, filename,
                                           (float)hostSampleRate,
                                           (uint32_t)hostNumChannels, fd, err);
    diag.ringOpened = (map != nullptr);
    diag.ringErrno  = err;
    if (!map) return;

    const juce::SpinLock::ScopedLockType sl(shmLock);
    shmMap       = map;
    shmFd        = fd;
    shmOpenedKey = resolvedDir + filename;  // full path for closeRing
}

void LinkProcessor::closeRingDeferred()
{
    void* old = nullptr;
    int   fd  = -1;
    juce::String key;
    {
        const juce::SpinLock::ScopedLockType sl(shmLock);
        old          = shmMap;
        fd           = shmFd;
        key          = shmOpenedKey;
        shmMap       = nullptr;
        shmFd        = -1;
        shmOpenedKey = {};
    }
    if (!old) return;

    // Unlink immediately → consumer's next probe sees "not found"
    LinkShm::closeRing(nullptr, -1, key, /*doUnlink=*/true);

    // Defer munmap 50 ms — audio thread may hold the old pointer inside produce()
    juce::Timer::callAfterDelay(50, [old, fd]()
    {
        LinkShm::closeRing(old, fd, {}, /*doUnlink=*/false);
    });
}

void LinkProcessor::closeRingNow()
{
    void* old = nullptr;
    int   fd  = -1;
    juce::String key;
    {
        const juce::SpinLock::ScopedLockType sl(shmLock);
        old          = shmMap;
        fd           = shmFd;
        key          = shmOpenedKey;
        shmMap       = nullptr;
        shmFd        = -1;
        shmOpenedKey = {};
    }
    if (old) LinkShm::closeRing(old, fd, key, /*doUnlink=*/true);
}

// =============================================================================
//  Public state machine (message thread)
// =============================================================================
void LinkProcessor::updateShmState()
{
    // Called after any linkName/linkOn change — tell the host non-parameter
    // state changed so it re-snapshots (plain updateHostDisplay() doesn't
    // signal this; a host restoring a stale blob would lose the name/toggle).
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));

    const bool on = linkOn.load();

    // A Link stays REGISTERED whether or not it is Active OR NAMED — the
    // main plugin must keep seeing it (heartbeat = listed): unnamed Links
    // show as "Untitled" rows and deactivated Links stay re-activatable
    // remotely. Only the audio RING (the capture/meter feed) is gated on
    // Active. Unnamed instances use a per-instance untitled file id so
    // their ring/registry filenames never collide.

    // Ensure dir is resolved and registry is open (sets resolvedDir)
    ensureRegistryOpen();

    // Registry: claim slot if not already claimed, or re-claim on name change
    if (regSlotIdx < 0)
    {
        claimRegistrySlot();
    }
    else
    {
        releaseRegistrySlot();
        claimRegistrySlot();
    }
    LinkShm::setSlotActive(regMap, regSlotIdx, on);
    // Re-publish gain + placement into the (possibly freshly claimed) slot so
    // the monitor shows the real values immediately, not the claim-time 0
    LinkShm::setSlotGain(regMap, regSlotIdx, gainDb_.load(std::memory_order_relaxed));
    LinkShm::setSlotPlacement(regMap, regSlotIdx,
                              (uint8_t) placement_.load(std::memory_order_relaxed));
    // THIS binary reads settings_structured and applies it (see
    // buildChainFromSpec), so it may say so. Published beside gain and
    // placement because it is the same kind of claim: a fact about this
    // writer, asserted by the writer, never inferred by the reader.
    LinkShm::setSlotDialCapable(regMap, regSlotIdx, true);

    if (on)
    {
        const juce::String wantedPath = resolvedDir.isEmpty() ? juce::String{}
            : resolvedDir + "audio_" + effectiveFilePart() + ".bin";

        // Ring: reopen if path changed or not yet open
        if (shmOpenedKey != wantedPath || shmMap == nullptr)
        {
            closeRingDeferred();
            openRing();
        }
    }
    else
    {
        closeRingDeferred();   // capture/meter role dormant
    }
}

// =============================================================================
//  AudioProcessor overrides
// =============================================================================
void LinkProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    rackMuteMix_.reset(sampleRate, 0.030);   // §8 mute ramp, click-free
    hostSampleRate  = sampleRate;
    // The ring is always stereo (mono is duplicated on write) so the monitor
    // reads a consistent 2-channel layout regardless of the track format.
    hostNumChannels = 2;
    const int block = samplesPerBlock > 0 ? samplesPerBlock : 512;
    meterEngine_.prepare(sampleRate, block);
    keyEngine_.prepare(sampleRate, block);   // resets its tap; continuous mode
                                             // re-accumulates from here

    // Chain hosting supports mono AND stereo. The hosted graph is stereo, so a
    // mono track is up-mixed (L duplicated to L+R) before the chain and folded
    // back down (0.5*(L+R)) after — stereo-only plugins run duplicated-mono,
    // native-mono plugins pass through unchanged. Only >2ch (surround /
    // ambisonic) layouts are unsupported and leave the chain out of circuit.
    const int nin  = getTotalNumInputChannels();
    const int nout = getTotalNumOutputChannels();
    chainSupported_.store(nin >= 1 && nin <= 2 && nout >= 1 && nout <= 2);
    chainMono_.store(nin == 1 || nout == 1);
    chainScratch_.setSize(2, block);
    chainHost.prepare(sampleRate, block);

    // Gain smoother — 30 ms ramp is short enough to feel immediate but long
    // enough to kill zipper noise on a fast drag. Snap to the current target
    // on prepare so a restored non-zero gain doesn't swell up from unity.
    gainSmoothed_.reset(sampleRate, 0.030);
    gainSmoothed_.setCurrentAndTargetValue(
        EchoJayFader::gainForDb(gainDb_.load(std::memory_order_relaxed)));
    gainSnapPending_.store(false, std::memory_order_relaxed);

    // Always sync shm state from the message thread so that:
    //  (a) a fresh session restore with Active=on registers correctly, and
    //  (b) a sample-rate change reopens the ring with the new rate.
    // updateShmState() is idempotent — if already open it compares the
    // wanted path against shmOpenedKey and skips the reopen.
    juce::MessageManager::callAsync([this] { updateShmState(); });
}

bool LinkProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Mono and stereo only (matches the main plugin). Rejects surround /
    // ambisonic so hosts don't offer exotic layouts the chain can't fold.
    auto in  = layouts.getMainInputChannelSet();
    auto out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return in == juce::AudioChannelSet::mono()
        || in == juce::AudioChannelSet::stereo();
}

void LinkProcessor::releaseResources()
{
    chainHost.release();
}

void LinkProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    audioBlockCounter_.fetch_add(1, std::memory_order_relaxed);   // audio liveness

    // Transport state for the passive key scheduler (§5.1): the rolling flag
    // gates arming, and a position landing far from where the last block left
    // off flags a section jump (§5.4 invalidation). One playhead query per
    // block, relaxed stores — the scheduler reads at 1 Hz.
    // Host sample position for the ring's position stamp (stage 0 of remote
    // editing). Captured from the SAME single per-block playhead query the
    // key scheduler uses; -1 = the host gave no position this block, and no
    // stamp is published, so absence stays absence (see LinkShm.h).
    int64_t stampHostPos = -1;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto ts = pos->getTimeInSamples())
                stampHostPos = *ts;
            const bool playingNow = pos->getIsPlaying();
            if (auto t = pos->getTimeInSeconds())
            {
                const double prev = transportTimeS_.load(std::memory_order_relaxed);
                if (playingNow && transportPlaying_.load(std::memory_order_relaxed)
                    && std::abs(*t - prev) > kKeyJumpSeconds)
                    keySectionJump_.store(true, std::memory_order_relaxed);
                transportTimeS_.store(*t, std::memory_order_relaxed);
            }
            transportPlaying_.store(playingNow, std::memory_order_relaxed);
        }
    }
    // Pass-through: silence extra output channels
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    // Hosted chain runs FIRST so the ring tap (and the track) hears the
    // processed signal. Empty / all-bypassed chain = the graph stays out of
    // circuit; only >2ch layouts skip it. On a mono track the signal is
    // up-mixed into the stereo graph then folded back to mono on output.
    if (chainSupported_.load(std::memory_order_relaxed))
    {
        const int n = buffer.getNumSamples();
        if (buffer.getNumChannels() == 1 && n <= chainScratch_.getNumSamples())
        {
            float* chans[2] = { chainScratch_.getWritePointer(0),
                                chainScratch_.getWritePointer(1) };
            const float* src = buffer.getReadPointer(0);
            juce::FloatVectorOperations::copy(chans[0], src, n);
            juce::FloatVectorOperations::copy(chans[1], src, n);
            juce::AudioBuffer<float> stereoView(chans, 2, n); // wraps scratch, no alloc
            chainHost.process(stereoView, midi);
            // Measure how much the chain decorrelated the duplicated-mono input:
            // |L-R| relative to total level (0 = mono-safe, →1 = fully stereo).
            // Leaky-averaged with a threshold + reset-on-silence so a transient
            // doesn't toggle the status note.
            {
                float diff = 0.0f, sum = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    diff += std::abs(chans[0][i] - chans[1][i]);
                    sum  += std::abs(chans[0][i]) + std::abs(chans[1][i]);
                }
                if (sum > 1.0e-4f)   // only when there's audible signal
                {
                    const float ratio = diff / sum;
                    monoDivergeAvg_ += 0.05f * (ratio - monoDivergeAvg_);
                    monoFoldDiverges_.store(monoDivergeAvg_ > 0.03f,
                                            std::memory_order_relaxed);
                }
            }
            // Fold to mono by TAKING LEFT — never sum. Summing can cancel a
            // widener / mid-side plugin to a quiet, hollow track (the worst
            // silent failure). Native-mono plugins keep L==R, so take-L is
            // lossless for them; a genuinely stereo plugin loses its right
            // output, which is surfaced once in the status line.
            float* dst = buffer.getWritePointer(0);
            juce::FloatVectorOperations::copy(dst, chans[0], n);
        }
        else
        {
            chainHost.process(buffer, midi);
        }
    }

    // Built-in gain stage — POST-chain, PRE-meter-tap. This position is
    // deliberate: (1) it gains the TRACK output (the gain is a real stage on
    // the signal path, so the DAW hears it), and (2) the meter tap below
    // reads the POST-gain signal, so the LINK monitor's integrated loudness
    // reflects the gain — which is exactly what the "match level" feature
    // needs to converge (target minus post-gain integrated IS the delta to
    // apply). Bit-transparent at 0 dB (unity multiply skipped entirely).
    applyGainSmoothed(buffer);

    // Metering for the LINK tab mini strips — Active only, POST-chain so
    // the meters read the processed signal (same tap point as the ring)
    if (linkOn.load(std::memory_order_acquire) && buffer.getNumChannels() >= 1)
    {
        const float* L = buffer.getReadPointer(0);
        const float* R = buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : L;
        meterEngine_.processBlock(L, R, buffer.getNumSamples());
        // Key tap, same gate and tap point: lock-free ring write, analysis
        // happens on the worker thread (KEY_DETECTOR_SPEC.md §9).
        keyEngine_.pushBlock(L, R, buffer.getNumSamples());
    }

    // Write into ring buffer if active -- non-blocking tryEnter. ALSO while
    // an edit lease is held: linkOn gates the ring, and an inactive Link
    // publishes nothing, but the whole point of the lease is that the main
    // plugin is monitoring this channel through its editing copy. Forcing
    // production here (rather than refusing to open editors on inactive
    // Links) keeps the feature available exactly when someone is mid-mix
    // with Links toggled off. Meters stay gated on linkOn alone.
    if (linkOn.load(std::memory_order_acquire)
        || leaseActive_.load(std::memory_order_relaxed))
    {
        if (shmLock.tryEnter())
        {
            if (shmMap != nullptr)
            {
                // Ring is always stereo: on a mono track duplicate L into R so
                // captures record and play back as proper (dual-mono) audio
                // instead of a dead right channel.
                const float* L = buffer.getNumChannels() >= 1 ? buffer.getReadPointer(0) : nullptr;
                const float* R = buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : L;
                const float* chPtrs[2] = { L, R };
                // The stamp pairs THIS block's first frame with the ring
                // index it is about to land on, published BEFORE the produce
                // so a reader can never see frames whose stamp has not been
                // written yet. No host position this block = no stamp.
                if (stampHostPos >= 0)
                {
                    LinkShm::ringStampPublish(shmMap,
                        LinkShm::loadRelaxed(&LinkShm::ringHeader(shmMap)->writeIdx),
                        stampHostPos);
                    // Log from the TIMER, not here: NSLog allocates and can
                    // block, and this is the audio thread. One relaxed store.
                    stampsArmed_.store(true, std::memory_order_relaxed);
                }
                LinkShm::ringProduce(shmMap, chPtrs, 2, buffer.getNumSamples());
                didWrite.store(true, std::memory_order_relaxed);
            }
            shmLock.exit();
        }
    }

    // §8 IN-CONTEXT MUTE, strictly AFTER the ring write: the ring must keep
    // carrying this signal (it is the main's injection source); only the
    // channel's contribution to the DAW mix goes silent. Ramped ~30ms so
    // engage/release never click. The want rides the lease (re-read every
    // poll; cleared by the one Release/Expire restore path), so a crash
    // un-mutes within the lease expiry exactly as it un-bypasses.
    {
        // Mute/solo layer (27 Aug 2026): THREE reasons, ONE ramp. The
        // lease mute, the user's own mute and the solo fabric compose
        // through linkMuteWanted(); each keeps its own lifetime, so a
        // session release can never clear a user mute.
        const bool muted = linkMuteWanted();
        rackMuteMix_.setTargetValue(muted ? 0.0f : 1.0f);
        if (muted || rackMuteMix_.getCurrentValue() < 0.9999f)
        {
            const int n = buffer.getNumSamples();
            for (int i = 0; i < n; ++i)
            {
                const float g = rackMuteMix_.getNextValue();
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    buffer.getWritePointer(ch)[i] *= g;
            }
        }
        else
            rackMuteMix_.skip(buffer.getNumSamples());
    }
}

// Audio thread. Glides the smoothed linear gain toward the atomic target and
// multiplies it into the buffer. Returns false (and touches nothing) when the
// stage is a settled unity no-op, so 0 dB is bit-transparent.
bool LinkProcessor::applyGainSmoothed(juce::AudioBuffer<float>& buffer)
{
    const float targetLin =
        EchoJayFader::gainForDb(gainDb_.load(std::memory_order_relaxed));

    if (gainSnapPending_.exchange(false, std::memory_order_acq_rel))
        gainSmoothed_.setCurrentAndTargetValue(targetLin);
    else
        gainSmoothed_.setTargetValue(targetLin);

    // Settled at unity → nothing to do (bit-transparent bypass at 0 dB)
    if (!gainSmoothed_.isSmoothing() && gainSmoothed_.getCurrentValue() == 1.0f)
        return false;

    const int n = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();
    if (gainSmoothed_.isSmoothing())
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = gainSmoothed_.getNextValue();
            for (int ch = 0; ch < numCh; ++ch)
                buffer.getWritePointer(ch)[i] *= g;
        }
    }
    else
    {
        buffer.applyGain(gainSmoothed_.getCurrentValue());
    }
    return true;
}

void LinkProcessor::setGainDb(float db, bool snapSmoothing)
{
    db = juce::jlimit(kGainMinDb, kGainMaxDb, db);
    gainDb_.store(db, std::memory_order_relaxed);
    if (snapSmoothing)
        gainSnapPending_.store(true, std::memory_order_relaxed);

    // Mirror to the registry slot so the monitor shows it (Active or not)
    if (regMap != nullptr && regSlotIdx >= 0)
        LinkShm::setSlotGain(regMap, regSlotIdx, db);

    // Non-parameter state changed — host re-snapshots (dirty-mark)
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));

    if (onLinkStateChanged) onLinkStateChanged();   // open editor slider sync
}

void LinkProcessor::setPlacement(int p)
{
    // Clamp to the LAST enum value. This clamped to PlacementInsert from
    // before Send existed (49125ff), so choosing Send stored Channel: the
    // menu, the remote command and the shm mirror all passed through here.
    // The restore path (setStateInformation) already clamps to Send.
    p = juce::jlimit((int) PlacementUnset, (int) PlacementSend, p);
    placement_.store(p, std::memory_order_relaxed);
    if (regMap != nullptr && regSlotIdx >= 0)
        LinkShm::setSlotPlacement(regMap, regSlotIdx, (uint8_t) p);
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
    if (onLinkStateChanged) onLinkStateChanged();
    EchoJay_NSLog(("EJLinkState: placement set to "
                   + juce::String(p == PlacementBus ? "bus"
                                  : p == PlacementInsert ? "insert"
                                  : p == PlacementSend ? "send" : "unset")).toRawUTF8());
}

// =============================================================================
//  Chain hosting (message thread)
// =============================================================================
juce::String LinkProcessor::chainFormatFilter() const
{
    switch (wrapperType)
    {
        case juce::AudioProcessor::wrapperType_AudioUnit: return "AudioUnit";
        case juce::AudioProcessor::wrapperType_VST3:      return "VST3";
        // AAX (Pro Tools, macOS only) takes the same filter as the AU host:
        // AU is the format that is both hostable and dialable on a Mac, so a
        // named case, not the old fallthrough (mirrors the main plugin's
        // chainFormatFilter_ switch in PluginEditor.cpp).
        case juce::AudioProcessor::wrapperType_AAX:       return "AudioUnit";
        default:                                          return {};
    }
}

juce::StringArray LinkProcessor::loadDisabledUids()
{
    // plugin_disabled.json — a JSON array of scanner uids
    // (lowercase name + "_" + lowercase manufacturer, spaces -> underscores).
    auto file = echojay::userAppData()
                    .getChildFile("Application Support/EchoJay/plugin_disabled.json");
    juce::StringArray uids;
    if (file.existsAsFile())
    {
        auto v = juce::JSON::parse(file.loadFileAsString());
        if (auto* arr = v.getArray())
            for (auto& u : *arr)
                uids.add(u.toString());
    }
    return uids;
}

juce::PluginDescription LinkProcessor::resolveChainPlugin(const juce::String& name) const
{
    // Shared loose resolver (identical behaviour to the main plugin):
    // exact -> parenthetical-stripped (+manufacturer tie-break) -> normalised.
    // The match path (or closest candidates) is logged per entry.
    juce::String matchLog;
    auto d = chainHost.resolveByName(name, chainFormatFilter(), &matchLog);
    EchoJay_NSLog(("EJChatLink: resolve \"" + name + "\" -- " + matchLog).toRawUTF8());
    return d;
}

void LinkProcessor::clearChainInternal()
{
    if (onChainAboutToChange) onChainAboutToChange();   // editors close first
    // Item 4: archive the Link's live rack before it is replaced, the same
    // protection the main rack gets. clearChainInternal is the whole-rack
    // replace point (per-slot edits use removeSlot directly), so this fires on
    // exactly the destructive event and no-ops on an empty rack.
    chainHost.archiveCurrentRack("chain replaced (Link)");
    for (int i = chainHost.getNumSlots() - 1; i >= 0; --i)
        chainHost.removeSlot(i);
    chainModel.clear();
}

void LinkProcessor::updateChainLatency()
{
    ejSetLatencyLogged (*this, chainHost.getTotalLatencySamples(), "LinkProcessor #1");
}

void LinkProcessor::resyncChainModelFromHost()
{
    std::vector<ChainSlotSpec> next;
    const int n = chainHost.getNumSlots();
    next.reserve((size_t)n);
    for (int i = 0; i < n; ++i)
    {
        auto info = chainHost.getSlotInfo(i);
        ChainSlotSpec s;
        s.name     = info.name;
        // THE GENERAL RULE (24 Aug 2026): lease bypass is the LEASE'S
        // business and never reaches the Link's own saved state — chainModel
        // is what the editor renders AND what persists, and a persisted
        // lease-bypass means a crash mid-borrow leaves the rack silently
        // switched off. Under a lease the model records the TRUE state (the
        // saved prior), not the lease's temporary dry rack.
        if (rackLeaseActive_ && i < (int) rackLeasePrior_.size())
            s.bypassed = rackLeasePrior_[(size_t) i];
        else if (! rackLeaseActive_ && leaseSlot0_ == i
                 && leaseActive_.load(std::memory_order_relaxed))
            s.bypassed = leasePriorBypass_;
        else
            s.bypassed = info.bypassed;
        s.hostIdx  = i;
        s.wet      = info.wet;
        s.settings = info.settings;
        if (s.settings.isEmpty())
            for (auto& old : chainModel)
                if (!old.missing && ChainHost::namesMatchLoose(old.name, s.name))
                { s.settings = old.settings; break; }
        next.push_back(std::move(s));
    }
    chainModel = std::move(next);
}

void LinkProcessor::rackLeaseEngage()
{
    // WHOLE-RACK ENGAGE: save every slot's bypass, bypass all once, stream
    // dry. setSlotBypassed bumps the revision, so the sidecar republishes
    // with every slot controlled. Extracted so linksync_test drives the
    // REAL arm, not a test-local copy.
    rackLeasePrior_.clear();
    for (int i = 0; i < chainHost.getNumSlots(); ++i)
    {
        rackLeasePrior_.push_back(chainHost.getSlotInfo(i).bypassed);
        chainHost.setSlotBypassed(i, true);
    }
    rackLeaseActive_ = true;
    leaseSlot0_      = -1;
    leaseActive_.store(true, std::memory_order_relaxed);
    notifyChainModel();
    EchoJay_NSLog(("EJLease: RACK engaged, " +
                   juce::String((int) rackLeasePrior_.size())
                   + " slot(s) bypassed, streaming dry").toRawUTF8());
}

void LinkProcessor::rackLeaseRelease()
{
    for (int i = 0; i < chainHost.getNumSlots()
                    && i < (int) rackLeasePrior_.size(); ++i)
        chainHost.setSlotBypassed(i, rackLeasePrior_[(size_t) i]);
    EchoJay_NSLog(("EJLease: RACK released/expired - "
                   + juce::String((int) rackLeasePrior_.size())
                   + " slot bypass state(s) restored").toRawUTF8());
    rackLeaseActive_ = false;
    rackLeasePrior_.clear();
    // The model re-reads the restored truth: without this, a sync that ran
    // mid-lease would leave the editor (and the SAVED state) claiming the
    // lease's bypass long after the lease ended.
    resyncChainModelFromHost();
}

void LinkProcessor::syncModelAfterStructuralChange()
{
    // THE FOUR-STEP WRITER, one author (24 Aug 2026): the chain-cmd path
    // always did these four; the plan path did only the last, so the
    // editor re-read a model nobody had changed.
    resyncChainModelFromHost();
    publishRackSidecar();
    updateChainLatency();
    notifyChainModel();
}

ChainHost::PlanResult LinkProcessor::applyStructurePlanAndSync(
    const juce::String& dir, const LinkShm::StructureEdit::Plan& plan)
{
    auto res = chainHost.applyStructurePlan(dir, plan);
    if (res.ok && rackLeaseActive_)
    {
        // PLAN-AWARE PRIOR REMAP (24 Aug 2026: every plugin came back
        // bypassed): the priors were captured per index before the plan,
        // and the plan shifts indices. Each prior follows the SLOT it
        // belongs to, through the plan's own finalOrigin record; a created
        // slot's prior is the bypass state the plan carried for it (read
        // from the slot, where Phase B just wrote it) — never a default
        // and never "not restored". Then the lease's dry rack is
        // re-asserted, created slots included.
        std::vector<bool> np;
        for (int i = 0; i < (int) res.finalOrigin.size(); ++i)
        {
            const int o = res.finalOrigin[(size_t) i];
            np.push_back(o >= 0 && o < (int) rackLeasePrior_.size()
                             ? (bool) rackLeasePrior_[(size_t) o]
                             : chainHost.getSlotInfo(i).bypassed);
        }
        rackLeasePrior_ = std::move(np);
        for (int i = 0; i < chainHost.getNumSlots(); ++i)
            chainHost.setSlotBypassed(i, true);
        EchoJay_NSLog(("EJLease: priors remapped through the plan ("
                       + juce::String((int) rackLeasePrior_.size())
                       + " slots), dry rack re-asserted").toRawUTF8());
    }
    // UNCONDITIONAL: applied changed the shape, a rollback tore it down
    // and rebuilt it — the editor-facing model is stale either way. Runs
    // AFTER the remap so the sync records the lease's TRUE priors.
    syncModelAfterStructuralChange();
    return res;
}

void LinkProcessor::notifyChainModel()
{
    updateMonoStereoOnlyNames();
    // Fresh chain → clear the stale fold-down flag; the audio thread re-sets it
    // within a few blocks if the new chain still decorrelates a mono input.
    monoFoldDiverges_.store(false, std::memory_order_relaxed);
    if (onChainModelChanged) onChainModelChanged();
    // Chain model changed → host should re-snapshot our state
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}

// Message thread. Names the non-bypassed hosted plugins whose main bus cannot
// run mono — the plausible culprits when the mono fold-down drops stereo work.
void LinkProcessor::updateMonoStereoOnlyNames()
{
    juce::StringArray names;
    const int n = chainHost.getNumSlots();
    for (int i = 0; i < n; ++i)
    {
        auto info = chainHost.getSlotInfo(i);
        if (info.bypassed) continue;
        auto* p = chainHost.getSlotProcessor(i);
        if (p == nullptr) continue;
        // Flip only the main bus to mono, preserving any aux/sidechain buses.
        auto layout = p->getBusesLayout();
        if (layout.inputBuses.size()  > 0) layout.inputBuses.getReference(0)  = juce::AudioChannelSet::mono();
        if (layout.outputBuses.size() > 0) layout.outputBuses.getReference(0) = juce::AudioChannelSet::mono();
        if (!p->checkBusesLayoutSupported(layout))
            names.add(info.name);
    }
    monoStereoOnlyNames_ = names.joinIntoString(", ");
}

juce::String LinkProcessor::getMonoFoldNote() const
{
    if (!chainMono_.load() || !monoFoldDiverges_.load(std::memory_order_relaxed))
        return {};
    if (monoStereoOnlyNames_.isEmpty())
        return "A stereo-only plugin; using its left output";
    const bool many = monoStereoOnlyNames_.contains(",");
    return monoStereoOnlyNames_ + (many ? " are stereo-only; using their left output"
                                        : " is stereo-only; using its left output");
}

void LinkProcessor::buildChainFromSpec(std::vector<ChainBuildItem> spec,
                                      ChainHost::LoadOrigin origin,
                                       std::function<void(const juce::StringArray&,
                                                          const juce::var&)> onDone)
{
    if ((int)spec.size() > kMaxChainSlots)
        spec.resize((size_t)kMaxChainSlots);

    chainBuilding = true;
    clearChainInternal();
    notifyChainModel();

    // Resolve against the SAME entries list the main plugin maintains: pick
    // up the shared chain_entries.xml if another host refreshed it. Own scan
    // is only the backstop when no cache exists at all.
    chainHost.maybeReloadEntriesCache();
    if (chainHost.getNumPlugins() == 0 && !chainHost.isScanning())
        chainHost.startScan();

    // Build-time diagnostics: list size, source, freshness
    {
        auto ec = ChainHost::getEntriesCacheFile();
        EchoJay_NSLog(("EJChatLink: build start -- list=" +
            juce::String(chainHost.getNumPlugins()) + " entries, format=" +
            chainFormatFilter() + ", cache=" + ec.getFullPathName() +
            (ec.existsAsFile()
                 ? " (mtime " + ec.getLastModificationTime().toISO8601(true) + ")"
                 : " (missing)") +
            (chainHost.isScanning() ? ", scanning" : "")).toRawUTF8());
    }

    auto results  = std::make_shared<juce::StringArray>();
    auto detail   = std::make_shared<juce::Array<juce::var>>();
    auto disabled = std::make_shared<juce::StringArray>(loadDisabledUids());
    auto items    = std::make_shared<std::vector<ChainBuildItem>>(std::move(spec));
    auto idx      = std::make_shared<int>(0);
    auto self     = this;   // processor outlives message-thread callbacks in-session

    auto isDisabled = [disabled](const juce::String& name)
    {
        // Key on both the raw name and the parenthetical-stripped one — the
        // AI may send "Name (Manufacturer)" while uids derive from the plain
        // scanner name ("name_manufacturer_...").
        auto mk = [](const juce::String& n)
            { return n.trim().toLowerCase().replaceCharacter(' ', '_') + "_"; };
        auto k1 = mk(name);
        auto k2 = mk(ChainHost::stripParenthetical(name));
        for (auto& uid : *disabled)
            if (uid.startsWith(k1) || uid.startsWith(k2)) return true;
        return false;
    };

    auto addDetail = [detail](const juce::String& name, const juce::String& kind,
                              const juce::String& info, const juce::String& resolvedName)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("name", name);
        o->setProperty("kind", kind);
        if (info.isNotEmpty())         o->setProperty("detail", info);
        if (resolvedName.isNotEmpty()) o->setProperty("resolvedName", resolvedName);
        detail->add(juce::var(o));
    };

    auto stepPtr = std::make_shared<std::function<void()>>();
    *stepPtr = [self, results, detail, items, idx, isDisabled, addDetail, stepPtr, onDone, origin]()
    {
        // Wait for an in-flight scan before resolving (poll, bounded)
        if (self->chainHost.isScanning())
        {
            juce::Timer::callAfterDelay(200, [stepPtr] { (*stepPtr)(); });
            return;
        }

        if (*idx >= (int)items->size())
        {
            self->chainBuilding = false;
            self->updateChainLatency();
            self->notifyChainModel();
            // The Link reports its own dial outcome now. Before this, a
            // channel build produced no dial line anywhere: the summary was
            // wired into the main plugin's editor, which this binary does not
            // compile, so a whole build path was unobservable by construction.
            self->chainHost.reportDialWhenSettled("EJDialSummary(Link): channel build");
            if (onDone) onDone(*results, juce::var(*detail));
            return;
        }

        int i = (*idx)++;
        auto& item = (*items)[(size_t)i];

        ChainSlotSpec slot;
        slot.name     = item.name;
        slot.settings = item.settings;
        slot.bypassed = item.bypassed;
        slot.wet      = item.wet;

        if (isDisabled(item.name))
        {
            slot.missing = true;
            self->chainModel.push_back(slot);
            results->add(item.name + ": skipped (disabled in Settings)");
            addDetail(item.name, "skipped", "disabled in Settings", {});
            self->notifyChainModel();
            (*stepPtr)();
            return;
        }

        auto desc = self->resolveChainPlugin(item.name);
        if (desc.name.isEmpty())
        {
            slot.missing = true;
            self->chainModel.push_back(slot);
            results->add(item.name + ": not found");
            addDetail(item.name, "not_found", {}, {});
            self->notifyChainModel();
            (*stepPtr)();
            return;
        }

        // Re-check disabled state against the RESOLVED name too — the loose
        // resolver can match variants the raw-name key check misses.
        if (isDisabled(desc.name))
        {
            slot.missing = true;
            self->chainModel.push_back(slot);
            results->add(item.name + ": skipped (disabled in Settings)");
            addDetail(item.name, "skipped", "disabled in Settings", desc.name);
            self->notifyChainModel();
            (*stepPtr)();
            return;
        }

        juce::String stateB64 = item.stateBase64;
        bool wantBypass = item.bypassed;
        juce::var structuredForSlot = item.structured;
        // Format preference applies to NEW instantiation only: restores
        // (stateBase64 present) keep the format their state was saved with —
        // AU and VST3 state blobs are not interchangeable.
        if (stateB64.isEmpty())
            desc = self->chainHost.preferInlineHostableDesc(desc);
        self->chainHost.loadPluginAsync(desc, origin,
            [self, results, addDetail, slot, stateB64, wantBypass, structuredForSlot, stepPtr,
             name = item.name, resolvedName = desc.name]
            (const juce::String& err) mutable
        {
            if (err.isNotEmpty())
            {
                slot.missing = true;
                self->chainModel.push_back(slot);
                results->add(name + ": failed (" + err + ")");
                addDetail(name, "load_failed", err, resolvedName);
            }
            else
            {
                int hostIdx = self->chainHost.getNumSlots() - 1;
                slot.hostIdx = hostIdx;
                self->chainHost.setSlotSettings(hostIdx, slot.settings);
                // Auto-apply, the same three lines the LOCAL build path has had
                // since the dial work shipped (PluginEditor loadChainFromJson).
                // setSlotStructuredSettings ignores a void/non-object, so a
                // prose-only slot is unchanged.
                if (structuredForSlot.getDynamicObject() != nullptr
                    || structuredForSlot.isArray())
                    self->chainHost.setSlotStructuredSettings(hostIdx, structuredForSlot);
                self->chainHost.setSlotWet(hostIdx, slot.wet, ChainHost::WetSource::Restore);
                if (wantBypass)
                    self->chainHost.setSlotBypassed(hostIdx, true);
                // Restore the hosted plugin's saved state (session restore)
                if (stateB64.isNotEmpty())
                {
                    juce::MemoryOutputStream mo;
                    if (juce::Base64::convertFromBase64(mo, stateB64))
                        if (auto* p = self->chainHost.getSlotProcessor(hostIdx))
                            p->setStateInformation(mo.getData(), (int)mo.getDataSize());
                }
                self->chainModel.push_back(slot);
                results->add(name + ": ok");
                addDetail(name, "built", {}, resolvedName);
            }
            self->notifyChainModel();
            (*stepPtr)();
        });
    };
    (*stepPtr)();
}

bool LinkProcessor::isPluginDisabledByName(const juce::String& name) const
{
    auto disabled = loadDisabledUids();
    auto mk = [](const juce::String& n)
        { return n.trim().toLowerCase().replaceCharacter(' ', '_') + "_"; };
    auto k1 = mk(name), k2 = mk(ChainHost::stripParenthetical(name));
    for (auto& uid : disabled)
        if (uid.startsWith(k1) || uid.startsWith(k2)) return true;
    return false;
}

void LinkProcessor::addChainPluginManually(const juce::PluginDescription& desc,
                                           std::function<void(const juce::String&)> done)
{
    if (rackLockGuard("add"))
    {
        if (done) done(rackEditPendingHeld()
            ? "An edit from \"" + rackLockOwner_ + "\" is still pending - "
              "retry or end the session there to release this rack."
            : "This rack is selected on \"" + rackLockOwner_
                       + "\" - deselect there to edit here.");
        return;
    }
    if ((int)chainModel.size() >= kMaxChainSlots)
    {
        if (done) done("Chain is full (" + juce::String(kMaxChainSlots) + " slots max)");
        return;
    }
    if (chainBuilding)
    {
        if (done) done("A chain build is in progress");
        return;
    }
    // NEW instantiation — popout-only AUs may swap to their VST3 build
    auto hostDesc = chainHost.preferInlineHostableDesc(desc);
    // USER: the sole caller is the Link's own plugin picker
    // (LinkEditor.cpp:516), which is a person choosing a plugin.
    chainHost.loadPluginAsync(hostDesc, ChainHost::LoadOrigin::User,
        [this, done, name = desc.name](const juce::String& err)
    {
        if (err.isNotEmpty()) { if (done) done(err); return; }
        // No settings guidance on manual adds — the empty string persists in
        // state like AI-provided guidance does (chainModelToVar serialises
        // the slot as-is), so the placeholder survives save/reopen.
        ChainSlotSpec slot;
        slot.name    = name;
        slot.hostIdx = chainHost.getNumSlots() - 1;
        chainModel.push_back(slot);
        stampLocalRackEdit();
        // Latency: chainHost.onChainChanged already ran updateChainLatency
        // during the graph rebuild. notifyChainModel refreshes the editor
        // and marks host state dirty — the same path command builds use.
        notifyChainModel();
        if (done) done({});
    });
}

void LinkProcessor::removeChainSlot(int idx)
{
    if (rackLockGuard("remove")) return;
    if (idx < 0 || idx >= (int)chainModel.size()) return;
    if (onChainAboutToChange) onChainAboutToChange();
    int hostIdx = chainModel[(size_t)idx].hostIdx;
    if (hostIdx >= 0)
    {
        chainHost.removeSlot(hostIdx);
        for (auto& s : chainModel)
            if (s.hostIdx > hostIdx) --s.hostIdx;
    }
    chainModel.erase(chainModel.begin() + idx);
    stampLocalRackEdit();
    notifyChainModel();
}

void LinkProcessor::moveChainSlot(int idx, int dir)
{
    if (rackLockGuard("move")) return;
    int j = idx + dir;
    if (idx < 0 || idx >= (int)chainModel.size()) return;
    if (j < 0 || j >= (int)chainModel.size()) return;
    // Host order = real model slots in order, so a host move is only needed
    // when BOTH swapped slots are real (adjacent real slots are adjacent in
    // the host too — missing slots don't exist there)
    auto& a = chainModel[(size_t)idx];
    auto& b = chainModel[(size_t)j];
    if (a.hostIdx >= 0 && b.hostIdx >= 0)
    {
        chainHost.moveSlot(a.hostIdx, dir);
        std::swap(a.hostIdx, b.hostIdx);
    }
    std::swap(a, b);
    stampLocalRackEdit();
    notifyChainModel();
}

void LinkProcessor::toggleChainSlotBypass(int idx)
{
    if (rackLockGuard("bypass")) return;
    if (idx < 0 || idx >= (int)chainModel.size()) return;
    auto& s = chainModel[(size_t)idx];
    s.bypassed = !s.bypassed;
    if (s.hostIdx >= 0)
        chainHost.setSlotBypassed(s.hostIdx, s.bypassed);
    stampLocalRackEdit();
    notifyChainModel();
}

void LinkProcessor::setChainSlotWet(int idx, float wet01)
{
    if (rackLockGuard("slot wet")) return;
    if (idx < 0 || idx >= (int)chainModel.size()) return;
    auto& s = chainModel[(size_t)idx];
    s.wet = juce::jlimit(0.0f, 1.0f, wet01);   // model copy = serialisation source
    if (s.hostIdx >= 0)
        chainHost.setSlotWet(s.hostIdx, s.wet, ChainHost::WetSource::Restore);
    stampLocalRackEdit();
}

float LinkProcessor::getChainSlotWet(int idx) const
{
    if (idx < 0 || idx >= (int)chainModel.size()) return 1.0f;
    return chainModel[(size_t)idx].wet;
}

void LinkProcessor::commitChainWetChange()
{
    // One host re-snapshot per gesture — same signal the other chain
    // mutations send via notifyChainModel, minus the model rebuild.
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}

// ---- Chain state serialise / restore ---------------------------------------
juce::var LinkProcessor::chainModelToVar() const
{
    juce::Array<juce::var> arr;
    for (auto& s : chainModel)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("name",     s.name);
        o->setProperty("settings", s.settings);
        o->setProperty("bypassed", s.bypassed);
        o->setProperty("missing",  s.missing);
        o->setProperty("wet",      (double)s.wet);
        if (!s.missing && s.hostIdx >= 0)
        {
            if (auto* p = chainHost.getSlotProcessor(s.hostIdx))
            {
                juce::MemoryBlock mb;
                p->getStateInformation(mb);
                if (mb.getSize() > 0)
                    o->setProperty("state", juce::Base64::toBase64(mb.getData(), mb.getSize()));
            }
        }
        arr.add(juce::var(o));
    }
    return juce::var(arr);
}

void LinkProcessor::restoreChainFromVar(const juce::var& v)
{
    auto* arr = v.getArray();
    if (arr == nullptr || arr->isEmpty()) return;

    std::vector<ChainBuildItem> spec;
    for (auto& sv : *arr)
    {
        auto* o = sv.getDynamicObject();
        if (o == nullptr) continue;
        ChainBuildItem item;
        item.name        = o->getProperty("name").toString();
        item.settings    = o->getProperty("settings").toString();
        item.structured  = o->getProperty("settings_structured");
        item.bypassed    = (bool)o->getProperty("bypassed");
        item.wet         = o->hasProperty("wet")
                             ? juce::jlimit(0.0f, 1.0f, (float)(double)o->getProperty("wet"))
                             : 1.0f;   // pre-wet/dry sessions restore fully wet
        item.stateBase64 = o->getProperty("state").toString();
        if (item.name.isNotEmpty())
            spec.push_back(std::move(item));
    }
    if (!spec.empty())
        // RESTORE: rebuilding a rack from a saved var, not a build.
        buildChainFromSpec(std::move(spec), ChainHost::LoadOrigin::Restore,
                           nullptr);   // missing → named slot, no crash
}

// =============================================================================
//  Chain transport — command/ack files in the shared link directory
// =============================================================================
juce::String LinkProcessor::chainInstanceId() const
{
    // The command/ack ADDRESS is the per-instance uid — never the name
    // (unnamed/duplicate names collapsed every same-named Link onto one
    // command file, so a toggle addressed to one applied to all)
    return instanceUid_;
}

void LinkProcessor::pollChainCommand()
{
    if (chainBuilding) return;                    // one build at a time
    auto id = chainInstanceId();
    if (id.isEmpty()) return;                     // unnamed Link — no identity

    if (resolvedDir.isEmpty())
    {
        int err = 0;
        resolvedDir = LinkShm::resolveDir(err);
        if (resolvedDir.isEmpty()) return;
    }

    juce::File cmdFile(resolvedDir + "chain-cmd-" + id + ".json");
    if (!cmdFile.existsAsFile()) return;

    auto v = juce::JSON::parse(cmdFile.loadFileAsString());
    auto* obj = v.getDynamicObject();
    if (obj == nullptr) { cmdFile.deleteFile(); return; }   // malformed — drop

    int ver = (int)obj->getProperty("v");
    int seq = (int)obj->getProperty("seq");
    if ((ver != 1 && ver != 2) || seq == lastAppliedChainSeq_ || seq == 0)
        return;   // unknown version or already applied — leave for inspection
                  // (older Links reject v:2 the same way: forward-safe)

    lastAppliedChainSeq_ = seq;
    cmdFile.deleteFile();   // consumed

    // ---- v:2 editOps (Phase 1c): structural edits on the EXISTING chain ----
    // Same shared sequencer as the main plugin (ChainHost::applyChainEdits,
    // staleness-guarded, stop-at-failure). Editors close first via
    // onChainAboutToChange, sequencer starts a beat later (AMEK discipline).
    if (ver == 2)
    {
        if (!obj->hasProperty("editOps"))
        { writeChainAck(seq, "failed", { "v2 command without editOps" }, {}); return; }
        // A leased rack refuses STRUCTURE. The lease saved one slot's bypass
        // state and will restore it BY INDEX; an edit that removes or moves
        // slots underneath it would make that restore hit the wrong plugin.
        // Refused with a reason, never queued.
        if (leaseActive_.load(std::memory_order_relaxed))
        { writeChainAck(seq, "failed",
              { "this rack is being edited from the main plugin - try again after release" },
              {}); return; }

        juce::StringArray baseSlots;
        if (auto* bs = obj->getProperty("baseSlots").getArray())
            for (auto& bv : *bs) baseSlots.add(bv.toString().trim());

        std::vector<ChainHost::ChainEditOp> ops;
        {
            // Reuse the parser via a synthetic payload so op-shape rules
            // stay single-source in ChainHost::parseChainEditOps
            auto* wrap = new juce::DynamicObject();
            wrap->setProperty("edit", obj->getProperty("editOps"));
            ops = ChainHost::parseChainEditOps(juce::JSON::toString(juce::var(wrap), true));
        }
        if (ops.empty())
        { writeChainAck(seq, "failed", { "editOps empty or malformed" }, {}); return; }

        if (onChainAboutToChange) onChainAboutToChange();   // editors close first
        auto self = this;   // processor outlives message-thread callbacks in-session
        juce::Timer::callAfterDelay(80, [self, ops, baseSlots, seq]() mutable
        {
            self->chainHost.applyChainEdits(std::move(ops), -1, baseSlots,
                [self, seq](const juce::StringArray& results, int applied, bool aborted)
            {
                self->syncModelAfterStructuralChange();
                juce::String status = aborted ? "stale"
                                    : (applied == results.size() ? "ok" : "partial");
                self->writeChainAck(seq, status, results, {});
            });
        });
        return;
    }

    std::vector<ChainBuildItem> spec;
    if (auto* arr = obj->getProperty("chain").getArray())
    {
        for (auto& ev : *arr)
        {
            if (auto* eo = ev.getDynamicObject())
            {
                ChainBuildItem item;
                item.name     = eo->getProperty("name").toString().trim();
                item.settings = eo->getProperty("settings").toString();
                // THE COMMAND PATH, and the one that matters (11 Aug 2026).
                // There are TWO parses of a chain array in this file and the
                // first fix went to the wrong one: restoreChainFromVar is
                // SESSION RESTORE, while THIS is what reads
                // chain-cmd-<uid>.json -- the file a Build button writes. The
                // payload arrived correct end to end (1370 ch, 5 slots, 5 with
                // settings_structured, confirmed on disk) and was dropped
                // here, one function away from the fix.
                item.structured = eo->getProperty("settings_structured");
                // Saved-chain recall to a Link (15 Aug 2026): the recall
                // sender includes per-slot hosted state and bypass - the
                // SAME fields session restore has always carried one
                // function up (restoreChainFromVar), applied by the same
                // buildChainFromSpec. Build commands never set them, so a
                // build turn parses exactly as before.
                item.stateBase64 = eo->getProperty("state").toString();
                item.bypassed    = (bool) eo->getProperty("bypassed");
                if (eo->hasProperty("wet"))
                    item.wet = juce::jlimit(0.0f, 1.0f,
                                            (float)(double) eo->getProperty("wet"));
                // The model's slot-level "wet_pct" (0..100) on a chain block
                // built to this Link (16 Aug 2026): same knob, same default;
                // a non-number is absent. "wet" (0..1, recall/restore) wins
                // if both are present, which no sender does.
                else if (eo->hasProperty("wet_pct"))
                {
                    const auto wv = eo->getProperty("wet_pct");
                    if (wv.isDouble() || wv.isInt() || wv.isInt64())
                        item.wet = juce::jlimit(0.0f, 1.0f, (float)(double) wv / 100.0f);
                }
                if (item.name.isNotEmpty())
                    spec.push_back(std::move(item));
            }
        }
    }

    // RECEIVE SIDE, mirroring the sender's line so the hop is closed at both
    // ends. The main plugin logs "sending to Link -- N slots, N with
    // settings_structured"; this says what arrived. Two numbers that disagree
    // localise the loss to the file or the parse without another round of
    // reasoning about which of them it must be.
    {
        int withStructured = 0;
        for (const auto& it : spec)
            if (it.structured.getDynamicObject() != nullptr || it.structured.isArray())
                ++withStructured;
        EchoJay_NSLog(("EJChain(Link): parsed command -- " + juce::String((int) spec.size())
                       + " slot(s), " + juce::String(withStructured)
                       + " with settings_structured").toRawUTF8());
    }

    // ASSISTANT: a build command arriving from the main plugin, which is
    // an EchoJay chain being placed on this channel.
    buildChainFromSpec(std::move(spec), ChainHost::LoadOrigin::Assistant,
        [this, seq](const juce::StringArray& results, const juce::var& detail)
    {
        int failures = 0;
        for (auto& r : results)
            if (!r.endsWith(": ok")) ++failures;
        juce::String status = !chainSupported_.load() ? "unsupported channel layout"
                            : failures == 0  ? "ok"
                            : failures == results.size() ? "failed" : "partial";
        writeChainAck(seq, status, results, detail);
    });
}

void LinkProcessor::writeChainAck(int seq, const juce::String& status,
                                  const juce::StringArray& results,
                                  const juce::var& detail)
{
    // PHASE 1b instrumentation: every chain ack says what it answered.
    EchoJay_NSLog(("EJChainAck[" + juce::String(seq) + "] link: status="
                   + status).toRawUTF8());
    if (resolvedDir.isEmpty()) return;
    auto id = chainInstanceId();
    if (id.isEmpty()) return;

    auto* obj = new juce::DynamicObject();
    obj->setProperty("v",      1);
    obj->setProperty("seq",    seq);
    obj->setProperty("status", status);
    juce::Array<juce::var> arr;
    for (auto& r : results) arr.add(r);
    obj->setProperty("perPluginResults", juce::var(arr));
    if (detail.isArray())
        obj->setProperty("perPluginDetail", detail);

    juce::File(resolvedDir + "chain-ack-" + id + ".json")
        .replaceWithText(juce::JSON::toString(juce::var(obj), true));
}

juce::AudioProcessorEditor* LinkProcessor::createEditor() { return new LinkEditor(*this); }

void LinkProcessor::getStateInformation(juce::MemoryBlock& dest)
{
#if ECHOJAY_LINK_STATE_DIAG
    EchoJay_NSLog(("EJLinkState: getState linkOn=" + juce::String((int)linkOn.load())
                   + " name=\"" + linkName + "\"").toRawUTF8());
#endif
    juce::DynamicObject* obj = new juce::DynamicObject();
    obj->setProperty("linkName", linkName);
    obj->setProperty("linkOn",   (bool)linkOn.load());
    obj->setProperty("gainDb",   (double)gainDb_.load(std::memory_order_relaxed));
    obj->setProperty("placement", placement_.load(std::memory_order_relaxed));
    obj->setProperty("projectName", projectName);
    obj->setProperty("genre",       genre);
    obj->setProperty("editorW",  editorW);
    obj->setProperty("editorH",  editorH);
    obj->setProperty("instanceUid", instanceUid_);
    {   // the author: this host process run (pid + start time), so a restore can
        // tell a from-disk reopen from a chunk the host re-applied in this run
        const auto& h = ChainHost::getHostIdentity();
        obj->setProperty("authorPid",       (int) ::getpid());
        obj->setProperty("authorStartSec",  (double) h.startSec);
        obj->setProperty("authorStartUsec", (double) h.startUsec);
    }
    // muteUser is channel mix identity and persists. soloOn is DELIBERATELY
    // ABSENT and must stay absent: a saved solo is how a project opens
    // silent and nobody knows why (MUTE_SOLO_SPEC §4; the gate pins this).
    obj->setProperty("muteUser", muteUserOn_.load(std::memory_order_relaxed));
    obj->setProperty("hostTrackName", getHostTrackName());
    // Full hosted chain: identities, order, bypass flags, wet mixes,
    // per-plugin state
    obj->setProperty("chain",    chainModelToVar());
    obj->setProperty("chainMasterWet", (double)chainHost.getMasterWet());
    juce::String json = juce::JSON::toString(juce::var(obj), true);
    dest.replaceAll(json.toRawUTF8(), json.getNumBytesAsUTF8());
}

void LinkProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    juce::String json = juce::String::fromUTF8(static_cast<const char*>(data), sizeInBytes);
    auto v = juce::JSON::parse(json);
    if (auto* obj = v.getDynamicObject())
    {
        // OUR OWN CHUNK RE-APPLIED (6 Sep 2026, the v6 regression): Pro Tools
        // calls SetChunk on a live instance repeatedly with that instance's own
        // current chunk (212 setState lines for ~40 instances in one session).
        // A chunk carrying the uid THIS instance already holds a registry slot
        // for is not a seed and not a restore: it is us. Identity stays settled
        // (no re-arm of chunkUid_/chunkAuthoredHere_ - the next re-claim would
        // find no holder for our own uid and re-mint, which burned 89 identities
        // in twenty minutes and dropped every host-delivered name), and name
        // PROVENANCE is not downgraded: a host-delivered or user-typed name
        // stays authoritative; the chunk's copy of a name fills in only where
        // we have none.
        const juce::String chunkUidIn = obj->getProperty("instanceUid").toString();
        const bool ownChunk = chunkUidIn.isNotEmpty() && chunkUidIn == instanceUid_ && regSlotIdx >= 0;
        if (obj->hasProperty("linkName"))
        {
            const auto n = obj->getProperty("linkName").toString();
            if (! ownChunk) { linkName = n; typedNameFromUser_ = false; }   // seeded: provisional
            else if (linkName.isEmpty()) linkName = n;                     // ours: fill only, keep provenance
        }
        if (obj->hasProperty("linkOn"))   linkOn.store((bool)obj->getProperty("linkOn"));
        if (obj->hasProperty("gainDb"))
            gainDb_.store(juce::jlimit(kGainMinDb, kGainMaxDb,
                                       (float)(double)obj->getProperty("gainDb")),
                          std::memory_order_relaxed);
        gainSnapPending_.store(true, std::memory_order_relaxed);  // restore: jump, don't swell
        if (obj->hasProperty("placement"))
            placement_.store(juce::jlimit((int)PlacementUnset, (int)PlacementSend,
                                          (int)obj->getProperty("placement")),
                             std::memory_order_relaxed);
        if (obj->hasProperty("projectName"))
            projectName = obj->getProperty("projectName").toString();
        if (obj->hasProperty("genre"))
            genre = obj->getProperty("genre").toString();
        if (! ownChunk)
        {
            if (chunkUidIn.isNotEmpty()) instanceUid_ = chunkUidIn;
            chunkUid_ = instanceUid_;
            const auto& h = ChainHost::getHostIdentity();
            chunkAuthoredHere_ = obj->hasProperty("authorPid")
                && (int) obj->getProperty("authorPid") == (int) ::getpid()
                && (juce::int64)(double) obj->getProperty("authorStartSec")  == h.startSec
                && (juce::int64)(double) obj->getProperty("authorStartUsec") == h.startUsec;
        }
        // ownChunk: identity untouched, nothing re-armed
        if (obj->hasProperty("muteUser"))
            muteUserOn_.store((bool) obj->getProperty("muteUser"),
                              std::memory_order_relaxed);
        if (obj->hasProperty("hostTrackName"))
        {
            // Seed the stash + dirty flag so the restored name shows and
            // publishes immediately, even in a host that never re-fires the
            // callback. setStateInformation is not guaranteed message-thread,
            // so this takes the same stash path as the live callback.
            {
                const juce::ScopedLock sl(hostNameLock_);
                if (! ownChunk) { hostTrackName_ = obj->getProperty("hostTrackName").toString(); hostNameFromHost_ = false; }   // seeded: PROVISIONAL
                else if (hostTrackName_.isEmpty()) hostTrackName_ = obj->getProperty("hostTrackName").toString();           // ours: fill only, keep provenance
            }
            hostNameDirty_.store(true, std::memory_order_release);
        }
        if (obj->hasProperty("chainMasterWet"))
            chainHost.setMasterWet((float)(double)obj->getProperty("chainMasterWet"));
#if ECHOJAY_LINK_STATE_DIAG
        EchoJay_NSLog(("EJLinkState: setState linkOn=" + juce::String((int)linkOn.load())
                       + " (hadProp=" + juce::String((int)obj->hasProperty("linkOn"))
                       + ") name=\"" + linkName + "\"").toRawUTF8());
#endif
        // An OPEN editor must sync its toggle to the restored value — a
        // stale ON toggle writing itself back into the processor is a
        // re-activation path. Message thread deferred: setStateInformation's
        // calling thread is host-defined.
        juce::MessageManager::callAsync([this]
        {
            updateShmState();
            if (onLinkStateChanged) onLinkStateChanged();
        });
        if (obj->hasProperty("editorW"))  editorW = juce::jlimit(900, 1800, (int)obj->getProperty("editorW"));
        if (obj->hasProperty("editorH"))  editorH = juce::jlimit(580, 1200, (int)obj->getProperty("editorH"));
        if (obj->hasProperty("chain"))
        {
            // Restore on the message thread — sequential async instantiation;
            // missing plugins become named empty slots, the rest still load.
            auto chainVar = obj->getProperty("chain");
            juce::MessageManager::callAsync([this, chainVar]
            {
                restoreChainFromVar(chainVar);
            });
        }
    }
    // Schedule registration on the message thread. If prepareToPlay has already
    // run (some hosts call it before setStateInformation), this triggers
    // registration immediately. If prepareToPlay comes later, the prepareToPlay
    // callAsync above will also call updateShmState — both are idempotent.
    juce::MessageManager::callAsync([this] { updateShmState(); });
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new LinkProcessor(); }
