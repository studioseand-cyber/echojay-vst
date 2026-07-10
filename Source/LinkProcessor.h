#pragma once
#include <JuceHeader.h>
#include "ChainHost.h"
#include "MeterEngine.h"
#include "LinkShm.h"     // LinkMeterFrame (frozen-engine publish guard member)
#include <atomic>
#include <functional>
#include <vector>

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

    // Session project name (no UI here): adopted from the shared
    // session_project.json when this instance has none of its own, follows
    // shared edits while it matches the previous shared value, serialised
    // with state. Own restored value always wins (never overwritten).
    juce::String projectName;

    // Session genre — same adopt/follow rules, for payload context. Empty
    // until a session genre exists (Link has no genre default or UI).
    juce::String genre;
    std::atomic<bool> didWrite  { false };  // set by audio thread on first successful produce

    /// Call from editor after any change to linkOn or linkName (message thread).
    void updateShmState();

    /// Fired on the message thread when linkOn/linkName change OUTSIDE the
    /// editor (state restore, remote ctrl command) so an open editor can
    /// sync its toggle — a stale ON toggle writing itself back into the
    /// processor is a re-activation path.
    std::function<void()> onLinkStateChanged;

    // ========================================================================
    // Chain hosting (phase 1) — chains arrive from the main plugin.
    // The MODEL is the requested chain including unresolvable slots
    // (missing=true, hostIdx=-1); ChainHost's slot order always equals the
    // real (non-missing) model slots in order.
    // ========================================================================
    struct ChainSlotSpec {
        juce::String name;
        juce::String settings;     // AI guidance text, display only
        bool bypassed = false;
        bool missing  = false;     // could not be resolved/loaded
        int  hostIdx  = -1;        // index into chainHost slots, -1 if missing
    };
    static constexpr int kMaxChainSlots = 15;

    struct ChainBuildItem {
        juce::String name;
        juce::String settings;
        bool bypassed = false;
        juce::String stateBase64;  // hosted plugin state (restore path only)
    };

    ChainHost& getChainHost() { return chainHost; }
    const std::vector<ChainSlotSpec>& getChainModel() const { return chainModel; }

    // Replace the chain with the given spec (message thread, sequential
    // instantiation, editors are NEVER opened during build). onDone receives
    // one result line per requested slot ("ok" / failure reason) plus a
    // structured array of {name, kind, detail, resolvedName} objects where
    // kind is "built" / "not_found" / "load_failed" / "skipped" — the sender
    // uses load_failed to offer the don't-suggest-again flow.
    void buildChainFromSpec(std::vector<ChainBuildItem> spec,
                            std::function<void(const juce::StringArray&,
                                               const juce::var&)> onDone);

    // Strip operations (message thread)
    void removeChainSlot(int idx);
    void moveChainSlot(int idx, int dir);      // dir: -1 / +1
    void toggleChainSlotBypass(int idx);

    // Manual add from the "+" picker (message thread). Appends to the current
    // chain (which may have arrived via command file); no settings guidance,
    // so the SUGGESTED SETTINGS card shows its placeholder. done(error) —
    // empty on success.
    void addChainPluginManually(const juce::PluginDescription& desc,
                                std::function<void(const juce::String&)> done);

    // plugin_disabled.json check for the picker — same keying as builds
    // (raw + parenthetical-stripped name against scanner uids)
    bool isPluginDisabledByName(const juce::String& name) const;

    // Active format filter for the picker ("AudioUnit" / "VST3" by wrapper)
    juce::String chainPickerFormat() const { return chainFormatFilter(); }

    // Fired on the message thread whenever the model changes (editor refresh)
    std::function<void()> onChainModelChanged;
    // Fired BEFORE slots are torn down so the editor can close hosted
    // editors first (one-editor-at-a-time discipline)
    std::function<void()> onChainAboutToChange;

    // True while a build/restore is in flight (strip shows progress)
    bool isChainBuilding() const { return chainBuilding; }

    // Stereo support flag — set in prepareToPlay; mono tracks pass through
    bool chainLayoutSupported() const { return chainStereoOk; }

    // Editor window size — persisted in plugin state
    int editorW = 1035, editorH = 638;

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

    // ---- Chain hosting internals (message thread unless noted) ----
    ChainHost chainHost;                    // audio-thread process() via processBlock
    std::vector<ChainSlotSpec> chainModel;
    bool chainBuilding = false;
    bool chainStereoOk = true;

    void clearChainInternal();              // closes editors first via callback
    void updateChainLatency();
    void notifyChainModel();
    juce::PluginDescription resolveChainPlugin(const juce::String& name) const;
    static juce::StringArray loadDisabledUids();
    juce::String chainFormatFilter() const; // "AudioUnit" / "VST3" by wrapper

    juce::var  chainModelToVar() const;     // state serialise (incl. plugin blobs)
    void       restoreChainFromVar(const juce::var& v);

    // ---- Chain transport: versioned command/ack files in the link dir ----
    // chain-cmd-<instanceId>.json  {v:1, seq, chain:[{name,role,settings}], sourceNote}
    // chain-ack-<instanceId>.json  {v:1, seq, status, perPluginResults,
    //                               perPluginDetail}
    // perPluginDetail is ADDITIVE (old senders ignore it, old Links omit it)
    // so the cmd "v" stays 1 — bumping it would make existing Links reject
    // new commands outright.
    // Polled at ~250ms on the message-thread timer; applied on seq change;
    // the command file is deleted on consume.
    int  lastAppliedChainSeq_ = 0;
    int  heartbeatDivider_ = 0;
    bool loggedInitState_ = false;   // item-1 diag: post-init log fired once

    // Per-Link metering for the main plugin's LINK tab mini strips. The
    // engine is fed on the audio thread (the same call the main plugin
    // makes) ONLY while Active; the 10Hz timer publishes a compact
    // LinkMeterFrame into the registry, also only while Active — an
    // inactive Link publishes nothing, so its strip freezes and dims.
    MeterEngine meterEngine_;
    int meterFramesPublished_ = 0;    // frame diagnostics counter
    LinkMeterFrame lastPublishedFrame_;   // frozen-engine guard (see publish)
    // Audio liveness: processBlock bumps this; the publisher marks frames
    // audioStale when it stops advancing for ~1s (Logic idles silent
    // channels — the engine freezes but heartbeats/timers keep running)
    std::atomic<uint32_t> audioBlockCounter_ { 0 };
    uint32_t lastSeenBlockCount_ = 0;
    uint32_t lastBlockAdvanceMs_ = 0;
    bool     audioWasStale_ = false;   // transition logging
    void publishMeterFrame();

    // Unnamed Links still register ("Untitled" rows): per-instance fallback
    // file id keeps their ring/registry filenames unique
    juce::String untitledId_;
    juce::String effectiveFilePart() const;
    juce::String chainInstanceId() const;
    void pollChainCommand();

    // Remote Active control: ctrl-cmd-<instanceId>.json {v:1, seq, active},
    // acked as ctrl-ack-<instanceId>.json {v:1, seq, active}. Authority is
    // this processor — the command goes through the same path as the local
    // toggle (linkOn + updateShmState + dirty-mark).
    int  lastAppliedCtrlSeq_ = 0;
    void pollControlCommand();

    // Session project/genre follow (see projectName/genre above)
    juce::String lastSeenSharedProject_;
    juce::String lastSeenSharedGenre_;
    void pollSessionProjectName();
    void writeChainAck(int seq, const juce::String& status,
                       const juce::StringArray& results,
                       const juce::var& detail);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LinkProcessor)
};
