#include "LinkProcessor.h"
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
    closeRingNow();
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
    if (++heartbeatDivider_ >= 30)
    {
        heartbeatDivider_ = 0;
        // Bump heartbeat so the consumer can detect we're alive vs. crashed.
        // Registered-but-inactive Links heartbeat too — they must stay
        // visible (not reaped as stale) so remote re-activation works.
        if (regSlotIdx >= 0)
        {
            LinkShm::bumpHeartbeat(regMap, regSlotIdx);
            // Mirror current heartbeat into diag for the editor to read
            if (regMap)
                diag.heartbeat = LinkShm::loadRelaxed(
                    &LinkShm::regSlots(regMap)[regSlotIdx].heartbeat);
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
        pollControlCommand();
        pollKeyCommand();
        pollSessionProjectName();
        publishRackSidecar();   // Phase R: revision-gated, usually a no-op
    }
    // Passive key detection duty cycle — once per second is plenty for a
    // scheduler whose shortest interval is 30 s.
    if (heartbeatDivider_ % 30 == 0)
        schedulePassiveKeyPass();
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
    const bool revMoved = (rev   != lastPublishedRackRev_);
    const bool epMoved  = (epoch != lastPublishedEpoch_);
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

    if (!revMoved && !epMoved && !curveMoved && !preGainMoved) return;
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
    lastPublishedRackRev_ = rev;
    lastPublishedEpoch_   = epoch;
    lastPublishedCurve_   = curve;
    lastPublishedPreGainDb_        = pgDb;
    lastPublishedPreGainUserSet_   = pgUser;
    lastPublishedPreGainInputKnown_ = pgKnown;
    lastRackPublishMs_    = nowMs;

    LinkShm::RackSidecar rc;
    rc.valid     = true;
    rc.uid       = instanceUid_;
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
                                     && i == leaseSlot0_,
                                 // The curve rides on the EQ's OWN slot, so a
                                 // reader never has to guess which entry it
                                 // describes. Every other slot leaves it empty.
                                 i == eqSlot ? curve : std::vector<int16_t>{} });
            // Identity so the slot can enter the server's fp union and dial.
            const auto id = chainHost.getSlotIdentity(i);
            auto& back = rc.slots.back();
            back.fp = id.fp; back.uid = id.uid; back.version = id.version;
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
void LinkProcessor::pollEditLease()
{
    // ~100ms cadence, message thread. The FILE is the lease (see LinkShm.h):
    // parse what is there, let the pure LeaseGate decide, act on the verdict.
    if (instanceUid_.isEmpty() || resolvedDir.isEmpty()) return;

    juce::String fileId;
    int          fileSlot1 = 0;
    double       ageMs     = 1.0e12;   // absent reads as infinitely stale
    juce::File f(LinkShm::leasePath(resolvedDir, instanceUid_));
    if (f.existsAsFile())
    {
        auto v = juce::JSON::parse(f.loadFileAsString());
        if (auto* o = v.getDynamicObject())
        {
            fileId    = o->getProperty("leaseId").toString();
            fileSlot1 = (int) o->getProperty("slot");
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
            const int slot0 = leaseGate_.activeSlot1 - 1;
            if (slot0 < 0 || slot0 >= chainHost.getNumSlots())
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
            // after a crash, a new id superseding a dead session. Restore
            // the bypass the user actually had, drop the flags, republish.
            leaseActive_.store(false, std::memory_order_relaxed);
            if (leaseSlot0_ >= 0 && leaseSlot0_ < chainHost.getNumSlots())
                chainHost.setSlotBypassed(leaseSlot0_, leasePriorBypass_);
            leaseSlot0_ = -1;
            notifyChainModel();
            EchoJay_NSLog("EJLease: released/expired - slot restored");
            break;
        }
        case LinkShm::LeaseGate::Hold:
        case LinkShm::LeaseGate::None:
            break;
    }
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

    auto v = juce::JSON::parse(cmdFile.loadFileAsString());
    auto* obj = v.getDynamicObject();
    if (obj == nullptr) { cmdFile.deleteFile(); return; }

    int ver = (int)obj->getProperty("v");
    int seq = (int)obj->getProperty("seq");
    if (ver != 1 || seq == lastAppliedCtrlSeq_ || seq == 0)
        return;

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
            else if ((int) mb.getSize() > ChainHost::kApiStateMaxSlotBytes)
                pullErr = "this plugin's settings are "
                        + juce::File::descriptionOfSizeInBytes((juce::int64) mb.getSize())
                        + ", over the "
                        + juce::File::descriptionOfSizeInBytes((juce::int64) ChainHost::kApiStateMaxSlotBytes)
                        + " limit, too large to carry";
            else
                pulledB64 = LinkShm::stateToB64(mb);
            // PHASE 1 BYTE ACCOUNTING (instrumentation, correlation id = seq).
            // Points 1 and 2 of four: raw size out of getStateInformation,
            // and bytes handed to the transport after encoding. The transport
            // is ONE ctrl-ack JSON file, NOT chunked, no framing beyond JSON
            // string quoting; its only cap is the raw-size gate above
            // (ChainHost::kApiStateMaxSlotBytes, enforced BEFORE encoding).
            EchoJay_NSLog(("EJPull[" + juce::String(seq) + "] link: raw="
                           + juce::String((juce::int64) mb.getSize())
                           + " bytes from getStateInformation; encoded="
                           + juce::String(pulledB64.length())
                           + " b64 chars; cap="
                           + juce::String(ChainHost::kApiStateMaxSlotBytes)
                           + " raw bytes; transport=single ctrl-ack JSON, unchunked"
                           + (pullErr.isNotEmpty() ? "; REFUSED: " + pullErr
                                                   : juce::String())).toRawUTF8());
        }
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
}

juce::String LinkProcessor::effectiveFilePart() const
{
    // Deliberately linkName-only (NOT effectiveDisplayName): a host track
    // name arriving or changing must never rename the audio ring file —
    // display is cosmetic, file identity stays stable.
    auto safe = LinkShm::makeSafeFilePart(linkName.trim());
    return safe.isNotEmpty() ? safe : "untitled_" + instanceUid_;
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
        if (hostTrackName_ == n) return;
        hostTrackName_ = n;
    }
    hostNameDirty_.store(true, std::memory_order_release);
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

    // Duplication guard: Logic's track-duplicate clones our serialised
    // state INCLUDING the uid — if another live slot already carries it,
    // regenerate ours so the two instances stay individually addressable
    {
        RegistrySlot* slots = LinkShm::regSlots(regMap);   // global-scope struct
        for (int i = 0; i < kRegMaxSlots; ++i)
            if (LinkShm::loadAcquire(&slots[i].inUse) != 0
                && instanceUid_ == juce::String::fromUTF8(slots[i].instanceUid))
            {
                auto old = instanceUid_;
                instanceUid_ = juce::String::toHexString(
                    juce::Random::getSystemRandom().nextInt64()).removeCharacters("-").substring(0, 10);
                EchoJay_NSLog(("EJLinkState: uid collision (duplicated instance?) "
                               + old + " -> regenerated " + instanceUid_).toRawUTF8());
                break;
            }
    }
    const juce::String audioFilename = "audio_" + effectiveFilePart() + ".bin";

    regSlotIdx = LinkShm::claimSlot(regMap,
                                     effectiveDisplayName(),
                                     audioFilename,
                                     instanceUid_,
                                     (float)hostSampleRate,
                                     (uint32_t)hostNumChannels);
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
    auto file = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
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
    setLatencySamples(chainHost.getTotalLatencySamples());
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
    *stepPtr = [self, results, detail, items, idx, isDisabled, addDetail, stepPtr, onDone]()
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
        self->chainHost.loadPluginAsync(desc,
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
                self->chainHost.setSlotWet(hostIdx, slot.wet);
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
    chainHost.loadPluginAsync(hostDesc,
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
        // Latency: chainHost.onChainChanged already ran updateChainLatency
        // during the graph rebuild. notifyChainModel refreshes the editor
        // and marks host state dirty — the same path command builds use.
        notifyChainModel();
        if (done) done({});
    });
}

void LinkProcessor::removeChainSlot(int idx)
{
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
    notifyChainModel();
}

void LinkProcessor::moveChainSlot(int idx, int dir)
{
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
    notifyChainModel();
}

void LinkProcessor::toggleChainSlotBypass(int idx)
{
    if (idx < 0 || idx >= (int)chainModel.size()) return;
    auto& s = chainModel[(size_t)idx];
    s.bypassed = !s.bypassed;
    if (s.hostIdx >= 0)
        chainHost.setSlotBypassed(s.hostIdx, s.bypassed);
    notifyChainModel();
}

void LinkProcessor::setChainSlotWet(int idx, float wet01)
{
    if (idx < 0 || idx >= (int)chainModel.size()) return;
    auto& s = chainModel[(size_t)idx];
    s.wet = juce::jlimit(0.0f, 1.0f, wet01);   // model copy = serialisation source
    if (s.hostIdx >= 0)
        chainHost.setSlotWet(s.hostIdx, s.wet);
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
        buildChainFromSpec(std::move(spec), nullptr);   // missing → named slot, no crash
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
                self->resyncChainModelFromHost();
                self->publishRackSidecar();   // Phase R: edits publish instantly
                self->updateChainLatency();
                self->notifyChainModel();
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

    buildChainFromSpec(std::move(spec),
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
        if (obj->hasProperty("linkName")) linkName = obj->getProperty("linkName").toString();
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
        if (obj->getProperty("instanceUid").toString().isNotEmpty())
            instanceUid_ = obj->getProperty("instanceUid").toString();
        if (obj->hasProperty("hostTrackName"))
        {
            // Seed the stash + dirty flag so the restored name shows and
            // publishes immediately, even in a host that never re-fires the
            // callback. setStateInformation is not guaranteed message-thread,
            // so this takes the same stash path as the live callback.
            {
                const juce::ScopedLock sl(hostNameLock_);
                hostTrackName_ = obj->getProperty("hostTrackName").toString();
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
