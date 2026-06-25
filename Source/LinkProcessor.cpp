#include "LinkProcessor.h"
#include "LinkEditor.h"
#include "LinkShm.h"

LinkProcessor::LinkProcessor()
    : AudioProcessor(BusesProperties()
          .withInput ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    startTimerHz(1); // heartbeat bump every second
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
    // Bump heartbeat so the consumer can detect we're alive vs. crashed
    if (linkOn.load() && regSlotIdx >= 0)
    {
        LinkShm::bumpHeartbeat(regMap, regSlotIdx);
        // Mirror current heartbeat into diag for the editor to read
        if (regMap)
            diag.heartbeat = LinkShm::loadRelaxed(
                &LinkShm::regSlots(regMap)[regSlotIdx].heartbeat);
    }
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

void LinkProcessor::claimRegistrySlot()
{
    ensureRegistryOpen();
    if (!regMap || regSlotIdx >= 0) return;

    const juce::String audioFilename = LinkShm::makeAudioFilename(linkName.trim());
    if (audioFilename.isEmpty()) return;

    regSlotIdx = LinkShm::claimSlot(regMap,
                                     linkName.trim(),
                                     audioFilename,
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
    const juce::String filename = LinkShm::makeAudioFilename(linkName.trim());
    if (filename.isEmpty()) return;

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
    const bool on = linkOn.load();
    const bool hasName = linkName.trim().isNotEmpty();
    const bool shouldBeActive = on && hasName;

    if (!shouldBeActive)
    {
        // Going inactive — release ring first, then registry slot
        closeRingDeferred();
        releaseRegistrySlot();
        return;
    }

    // Ensure dir is resolved and registry is open (sets resolvedDir)
    ensureRegistryOpen();

    const juce::String wantedPath = resolvedDir.isEmpty() ? juce::String{}
        : resolvedDir + LinkShm::makeAudioFilename(linkName.trim());

    // Ring: reopen if path changed or not yet open
    if (shmOpenedKey != wantedPath || shmMap == nullptr)
    {
        closeRingDeferred();
        openRing();
    }

    // Registry: claim slot if not already claimed, or if name changed
    if (regSlotIdx < 0)
    {
        claimRegistrySlot();
    }
    else
    {
        // Re-check that the slot still reflects the current name
        // (handles name change while active)
        releaseRegistrySlot();
        claimRegistrySlot();
    }
}

// =============================================================================
//  AudioProcessor overrides
// =============================================================================
void LinkProcessor::prepareToPlay(double sampleRate, int numChannels)
{
    hostSampleRate  = sampleRate;
    hostNumChannels = juce::jmin(numChannels, 2);

    // Reopen ring with new sample rate if currently active
    if (linkOn.load() && linkName.trim().isNotEmpty())
    {
        closeRingDeferred();
        openRing();
        // Update registry slot with new sr
        if (regSlotIdx >= 0 && regMap)
        {
            LinkShm::releaseSlot(regMap, regSlotIdx);
            regSlotIdx = -1;
            claimRegistrySlot();
        }
    }
}

void LinkProcessor::releaseResources() {}

void LinkProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    // Pass-through: silence extra output channels
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

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

juce::AudioProcessorEditor* LinkProcessor::createEditor() { return new LinkEditor(*this); }

void LinkProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    juce::DynamicObject* obj = new juce::DynamicObject();
    obj->setProperty("linkName", linkName);
    obj->setProperty("linkOn",   (bool)linkOn.load());
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
    }
    // prepareToPlay will open the ring/registry when the host initialises audio
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new LinkProcessor(); }
