#pragma once
#include <JuceHeader.h>
#include <atomic>

class LinkProcessor : public juce::AudioProcessor,
                      private juce::Timer
{
public:
    LinkProcessor();
    ~LinkProcessor() override;

    // Audio
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // Editor
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // Metadata
    const juce::String getName() const override { return "EchoJay Link"; }
    bool acceptsMidi()  const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // Programs (unused)
    int  getNumPrograms()                               override { return 1; }
    int  getCurrentProgram()                            override { return 0; }
    void setCurrentProgram(int)                         override {}
    const juce::String getProgramName(int)              override { return "Default"; }
    void changeProgramName(int, const juce::String&)    override {}

    // State
    void getStateInformation(juce::MemoryBlock& dest) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Link state (accessed by editor on message thread)
    juce::String      linkName;
    std::atomic<bool> linkOn    { false };
    std::atomic<bool> didWrite  { false };  // set by audio thread on first successful produce

    /// Call from editor after any change to linkOn or linkName (message thread).
    void updateShmState();

    // ---- Diagnostics (message thread, read by editor timer) ----
    struct Diag {
        juce::String regKey;          // resolved shared directory path
        bool         regOpened = false;
        int          regErrno  = 0;
        int          slotIdx   = -1;  // -1 = not claimed
        bool         ringOpened = false;
        int          ringErrno  = 0;
        uint32_t     heartbeat  = 0;
    };
    Diag diag;

private:
    // juce::Timer — bumps registry heartbeat once per second while active
    void timerCallback() override;

    // Host audio format — stored in prepareToPlay, used when opening the ring
    double hostSampleRate  = 44100.0;
    int    hostNumChannels = 2;

    // Resolved shared directory (message thread, set once in ensureRegistryOpen)
    juce::String   resolvedDir;

    // Audio ring buffer (audio thread writes via shmLock)
    juce::SpinLock shmLock;
    void*          shmMap       = nullptr;
    int            shmFd        = -1;
    juce::String   shmOpenedKey;  // full path (dir + filename) of open ring file

    void openRing();
    void closeRingDeferred();  // unlinks immediately, defers munmap 50 ms
    void closeRingNow();       // synchronous — destructor only

    // Registry (message thread only — no audio-thread access)
    void*  regMap    = nullptr;
    int    regFd     = -1;
    int    regSlotIdx = -1;

    void ensureRegistryOpen();
    void claimRegistrySlot();
    void releaseRegistrySlot();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LinkProcessor)
};
