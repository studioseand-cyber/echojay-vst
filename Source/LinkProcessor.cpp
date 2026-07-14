#include "LinkProcessor.h"
#include "LinkEditor.h"
#include "LinkShm.h"
#include "NativeClip.h"   // EchoJay_NSLog — chain-build diagnostics

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
    startTimerHz(10); // command polling + 10Hz meter-frame publish; heartbeat every 10th tick

    // Mirror hosted chain latency into the host on EVERY chain change —
    // Link sits on parallel and phase-critical tracks, so this must track
    // build/add/remove/bypass exactly.
    chainHost.onChainChanged = [this] { updateChainLatency(); };
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
    // Heartbeat once per second (timer runs at 10Hz: command polling +
    // meter-frame publish)
    if (++heartbeatDivider_ >= 10)
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

    pollChainCommand();
    pollControlCommand();
    pollSessionProjectName();
    publishMeterFrame();
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
    f.audioBlocks = blocksNow;
    f.audioStale  = audioStale ? 1u : 0u;
    if (audioStale)
    {
        // Momentary group -> absent convention (dashes); rms/peak persist
        // as last-programme values and the receiver dims them
        f.momentary   = -100.0f;
        f.shortTerm   = -100.0f;
        f.shortTermTP = -100.0f;   // gates PSR to '--' like the Meters tab
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
    // Frozen-engine guard: when the transport stops, the host stops calling
    // processBlock, the engine's values freeze, and stamping them would keep
    // the strip "fresh" scrolling a constant plateau forever — fake motion.
    // Real audio NEVER produces two byte-identical frames (LUFS jitters), so
    // an identical payload means frozen: skip the publish, seq stops
    // advancing, and the receiver's staleness path freezes+dims the strip.
    if (std::memcmp(reinterpret_cast<const uint8_t*>(&f) + sizeof(uint32_t),
                    reinterpret_cast<const uint8_t*>(&lastPublishedFrame_) + sizeof(uint32_t),
                    sizeof(LinkMeterFrame) - sizeof(uint32_t)) == 0)
        return;
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

    auto* ack = new juce::DynamicObject();
    ack->setProperty("v",         1);
    ack->setProperty("seq",       seq);
    ack->setProperty("active",    linkOn.load());
    ack->setProperty("gainDb",    (double)gainDb_.load(std::memory_order_relaxed));
    ack->setProperty("placement", placement_.load(std::memory_order_relaxed));
    juce::File(resolvedDir + "ctrl-ack-" + id + ".json")
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
    auto safe = LinkShm::makeSafeFilePart(linkName.trim());
    return safe.isNotEmpty() ? safe : "untitled_" + instanceUid_;
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
                                     linkName.trim(),
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
void LinkProcessor::prepareToPlay(double sampleRate, int numChannels)
{
    hostSampleRate  = sampleRate;
    hostNumChannels = juce::jmin(numChannels, 2);
    meterEngine_.prepare(sampleRate, getBlockSize() > 0 ? getBlockSize() : 512);

    // Chain hosting: stereo only. On mono tracks the chain stays out of
    // circuit (clean passthrough) and the transport ack reports it.
    chainStereoOk = (getTotalNumInputChannels() >= 2
                     && getTotalNumOutputChannels() >= 2);
    chainHost.prepare(sampleRate, getBlockSize() > 0 ? getBlockSize() : 512);

    // Gain smoother — 30 ms ramp is short enough to feel immediate but long
    // enough to kill zipper noise on a fast drag. Snap to the current target
    // on prepare so a restored non-zero gain doesn't swell up from unity.
    gainSmoothed_.reset(sampleRate, 0.030);
    gainSmoothed_.setCurrentAndTargetValue(
        juce::Decibels::decibelsToGain(gainDb_.load(std::memory_order_relaxed)));
    gainSnapPending_.store(false, std::memory_order_relaxed);

    // Always sync shm state from the message thread so that:
    //  (a) a fresh session restore with Active=on registers correctly, and
    //  (b) a sample-rate change reopens the ring with the new rate.
    // updateShmState() is idempotent — if already open it compares the
    // wanted path against shmOpenedKey and skips the reopen.
    juce::MessageManager::callAsync([this] { updateShmState(); });
}

void LinkProcessor::releaseResources()
{
    chainHost.release();
}

void LinkProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    audioBlockCounter_.fetch_add(1, std::memory_order_relaxed);   // audio liveness
    // Pass-through: silence extra output channels
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    // Hosted chain runs FIRST so the ring tap (and the track) hears the
    // processed signal. Empty / all-bypassed chain = the graph stays out of
    // circuit entirely; mono layouts skip the chain (stereo-only phase 1).
    if (chainStereoOk)
        chainHost.process(buffer, midi);

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
    }

    // Write into ring buffer if active — non-blocking tryEnter
    if (linkOn.load(std::memory_order_acquire))
    {
        if (shmLock.tryEnter())
        {
            if (shmMap != nullptr)
            {
                const float* chPtrs[2] = {
                    buffer.getNumChannels() >= 1 ? buffer.getReadPointer(0) : nullptr,
                    buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : nullptr
                };
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
        juce::Decibels::decibelsToGain(gainDb_.load(std::memory_order_relaxed));

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
    p = juce::jlimit((int) PlacementUnset, (int) PlacementInsert, p);
    placement_.store(p, std::memory_order_relaxed);
    if (regMap != nullptr && regSlotIdx >= 0)
        LinkShm::setSlotPlacement(regMap, regSlotIdx, (uint8_t) p);
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
    if (onLinkStateChanged) onLinkStateChanged();
    EchoJay_NSLog(("EJLinkState: placement set to "
                   + juce::String(p == PlacementBus ? "bus"
                                  : p == PlacementInsert ? "insert" : "unset")).toRawUTF8());
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
    for (int i = chainHost.getNumSlots() - 1; i >= 0; --i)
        chainHost.removeSlot(i);
    chainModel.clear();
}

void LinkProcessor::updateChainLatency()
{
    setLatencySamples(chainHost.getTotalLatencySamples());
}

void LinkProcessor::notifyChainModel()
{
    if (onChainModelChanged) onChainModelChanged();
    // Chain model changed → host should re-snapshot our state
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
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
            if (onDone) onDone(*results, juce::var(*detail));
            return;
        }

        int i = (*idx)++;
        auto& item = (*items)[(size_t)i];

        ChainSlotSpec slot;
        slot.name     = item.name;
        slot.settings = item.settings;
        slot.bypassed = item.bypassed;

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
        // Format preference applies to NEW instantiation only: restores
        // (stateBase64 present) keep the format their state was saved with —
        // AU and VST3 state blobs are not interchangeable.
        if (stateB64.isEmpty())
            desc = self->chainHost.preferInlineHostableDesc(desc);
        self->chainHost.loadPluginAsync(desc,
            [self, results, addDetail, slot, stateB64, wantBypass, stepPtr,
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
        item.bypassed    = (bool)o->getProperty("bypassed");
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
    if (ver != 1 || seq == lastAppliedChainSeq_ || seq == 0)
        return;   // unknown version or already applied — leave for inspection

    lastAppliedChainSeq_ = seq;
    cmdFile.deleteFile();   // consumed

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
                if (item.name.isNotEmpty())
                    spec.push_back(std::move(item));
            }
        }
    }

    buildChainFromSpec(std::move(spec),
        [this, seq](const juce::StringArray& results, const juce::var& detail)
    {
        int failures = 0;
        for (auto& r : results)
            if (!r.endsWith(": ok")) ++failures;
        juce::String status = !chainStereoOk ? "unsupported channel layout"
                            : failures == 0  ? "ok"
                            : failures == results.size() ? "failed" : "partial";
        writeChainAck(seq, status, results, detail);
    });
}

void LinkProcessor::writeChainAck(int seq, const juce::String& status,
                                  const juce::StringArray& results,
                                  const juce::var& detail)
{
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
    // Full hosted chain: identities, order, bypass flags, per-plugin state
    obj->setProperty("chain",    chainModelToVar());
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
            placement_.store(juce::jlimit((int)PlacementUnset, (int)PlacementInsert,
                                          (int)obj->getProperty("placement")),
                             std::memory_order_relaxed);
        if (obj->hasProperty("projectName"))
            projectName = obj->getProperty("projectName").toString();
        if (obj->hasProperty("genre"))
            genre = obj->getProperty("genre").toString();
        if (obj->getProperty("instanceUid").toString().isNotEmpty())
            instanceUid_ = obj->getProperty("instanceUid").toString();
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
