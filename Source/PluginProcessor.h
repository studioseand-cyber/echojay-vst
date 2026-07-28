#pragma once
#include <JuceHeader.h>
#include <thread>
#include <atomic>
#include "MeterEngine.h"
#include "PluginScanner.h"
#include "ReferenceAnalyser.h"
#include "WaveformRecorder.h"
#include "ChainHost.h"
#include "LinkShm.h"

// Temporary diagnostic: append a timestamped line to the EchoJay teardown log
// file (Release-safe; DBG is compiled out of Release). Used to trace the
// Cubase/Windows freeze-on-removal across both the processor and editor
// destructors. Defined in PluginProcessor.cpp. Remove once the freeze is fixed.
void ejTeardownLog(const juce::String& msg);


enum class ChannelType {
    FullMix = 0,
    // Vocals
    LeadVocal, BackingVocal, Adlibs, VocalBus,
    // Drums
    Kick, Snare, HiHat, Overheads, DrumBus, Percussion,
    // Bass
    Bass808, BassGuitar, SubBass, SynthBass,
    // Keys & Guitar
    Piano, Keys, AcousticGuitar, ElectricGuitar, GuitarBus,
    // Synths
    SynthLead, SynthPad, SynthPluck, SynthBus,
    // Strings & Brass
    Strings, Brass, Woodwind, Orchestral,
    // FX & Other
    FX, Reverb, Delay, Foley, Ambient,
    // Buses
    MasterBus, InstrumentBus, MusicBus,
    // Custom
    Other
};

static const juce::StringArray channelTypeNames = {
    "Mix Bus",
    "Lead Vocal", "Backing Vocal", "Adlibs", "Vocal Bus",
    "Kick", "Snare", "Hi-Hat", "Overheads", "Drum Bus", "Percussion",
    "Bass / 808", "Bass Guitar", "Sub Bass", "Synth Bass",
    "Piano", "Keys", "Acoustic Guitar", "Electric Guitar", "Guitar Bus",
    "Synth Lead", "Synth Pad", "Synth Pluck", "Synth Bus",
    "Strings", "Brass", "Woodwind", "Orchestral",
    "FX", "Reverb", "Delay", "Foley", "Ambient",
    "Master Bus", "Instrument Bus", "Music Bus",
    "Other"
};

enum class CaptureState { Idle, Capturing, Complete };

// Per-channel data from a multi-channel capture.
// channels[0] = host, channels[1..] = active Links.
// Only populated when Links were active during capture.
struct ChannelMeterData {
    juce::String name;
    juce::String uid;           // Link instance uid ("" = host) — live-name key
    MeterData    meterData;
    juce::String wavFilePath;   // filled by background save thread
    // Per-channel waveform thumbnail (Phase C handshake): captured
    // SYNCHRONOUSLY at finalizeLinkChannel from this channel's OWN recorder,
    // never the host's, so the card's picture is the channel's from the
    // first frame. Empty when no channel frames arrived.
    std::vector<WaveformRecorder::ThumbnailPoint> thumbnail;
    // Frames sentinel (capture honesty): -1 = host / not applicable;
    // 0 = NO frames arrived from this Link during the window (cause unknown,
    // never a claim about sound); >0 = real audio was received (silent
    // values are then a genuine fact).
    int64_t      framesReceived = -1;
};

struct CaptureSnapshot {
    juce::String id;
    juce::String name;
    ChannelType channelType;
    juce::String customChannelName;
    MeterData averagedData;
    juce::int64 timestamp;
    float durationSeconds;
    std::vector<float> waveformThumbnail;
    std::array<float, 64> eqCurve = {};
    juce::String wavFilePath;
    // Multi-channel data — empty when no Links active during capture
    std::vector<ChannelMeterData> channels;

    // Both spectra preserved for channel-shape / per-band crest analysis.
    // peakSpectrum  = max-hold dB per bin over the capture (transient character)
    // avgSpectrum   = average dB per bin over the capture (sustained character)
    // Per-band crest = peak - avg, used to distinguish transient vs sustained content.
    // Note: existing eqCurve / averagedData.spectrum may be either peak or avg
    // depending on channel type — those are kept for display/compatibility.
    std::array<float, 64> peakSpectrum = {};
    std::array<float, 64> avgSpectrum  = {};
    bool hasDualSpectrum = false; // false on snapshots restored from older save files
    
    juce::String getChannelDisplayName() const {
        if (channelType == ChannelType::Other && customChannelName.isNotEmpty())
            return customChannelName;
        return channelTypeNames[(int)channelType];
    }
};

class EchoJayProcessor : public juce::AudioProcessor
{
public:
    EchoJayProcessor();
    ~EchoJayProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // Host audio liveness: bumps every processBlock. Compare playback
    // renders INSIDE processBlock, so when the host idles the channel
    // (Logic, stopped transport) nothing can sound — the editor reads this
    // to keep the transport UI honest instead of pretending to play.
    uint32_t getAudioBlockCount() const { return audioBlocksProcessed_.load(std::memory_order_relaxed); }

    MeterEngine& getMeterEngine()   { return meterEngine; }
    MeterEngine& getABMeterEngine() { return abMeterEngine; }
    MeterEngine& getCompareMeter(int slot) { return (slot == 0) ? cmpMeter[0] : cmpMeter[1]; }
    PluginScanner& getPluginScanner() { return pluginScanner; }
    ReferenceAnalyser& getReferenceAnalyser() { return refAnalyser; }
    WaveformRecorder& getWaveformRecorder() { return waveformRecorder; }
    ChainHost& getChainHost() { return chainHost; }

    // Save captured audio to WAV in the project/capture folder
    juce::String saveCaptureWAV();
    juce::File getCaptureFolder() const;
    
    // Build AI compare context between a capture and a reference
    juce::String buildCompareContext(const CaptureSnapshot& capture, const ReferenceResult& reference) const;
    // Build AI compare context between two captures
    juce::String buildCompareContext(const CaptureSnapshot& a, const CaptureSnapshot& b) const;
    // Build AI compare context between two references
    juce::String buildCompareContext(const ReferenceResult& a, const ReferenceResult& b) const;

    // Tell the host non-parameter state changed so it re-snapshots our state
    // (plain updateHostDisplay() does NOT signal this — Logic could restore a
    // stale pre-selection blob if its hosting process recycles, re-showing
    // the channel/genre prompts the user already answered).
    void markStateDirty() { updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true)); }

    ChannelType getChannelType() const { return channelType; }
    void setChannelType(ChannelType t);
    juce::String getCustomChannelName() const { return customChannelName; }
    void setCustomChannelName(const juce::String& name) { customChannelName = name; markStateDirty(); }
    juce::String getEffectiveChannelName() const;

    bool isChannelTypePromptDismissed() const { return channelTypePromptDismissed; }
    void setChannelTypePromptDismissed(bool dismissed);

    juce::String getGenre() const { return genre; }
    void setGenre(const juce::String& g) { genre = g; markStateDirty(); }

    // Genre prompt answered — PROCESSOR-owned and serialised (like the
    // channel flag) so an editor rebuild or project reload never re-prompts.
    // Genre's default ("hip-hop") is never empty, so the value alone cannot
    // distinguish a fresh instance from a chosen one; this flag is the truth.
    bool isGenrePromptDismissed() const { return genrePromptDismissed; }
    void setGenrePromptDismissed(bool dismissed) { genrePromptDismissed = dismissed; markStateDirty(); }

    // Project-name prompt answered/skipped — same ownership rules. The
    // prompt only ever shows when the instance has no own name AND no shared
    // session value exists (see the editor's session-project flow).
    bool isProjectPromptDismissed() const { return projectPromptDismissed; }
    void setProjectPromptDismissed(bool dismissed) { projectPromptDismissed = dismissed; markStateDirty(); }

    // Project name — optional label for the current mix session.
    // Resetting the name to something new resets captureVersion to 1.
    juce::String getProjectName() const { return projectName; }
    void setProjectName(const juce::String& name);
    int  getCaptureVersion() const { return captureVersion; }

    // Returns the pass name used for WAV, review fileName, and chat title.
    // "Pass N" when no project is set; "project vN" when one is.
    juce::String computePassName() const;

    CaptureState getCaptureState() const { return captureState.load(); }
    void startCapture();
    void stopCapture();
    void resetCapture();
    float getCaptureDuration() const;

    std::vector<CaptureSnapshot> getSnapshots() const;
    void renameSnapshot(int index, const juce::String& newName);
    void deleteSnapshot(int index);
    CaptureSnapshot getLatestSnapshot() const;
    // Phase C handshake: fired on the MESSAGE THREAD when the background WAV
    // save completes (host + every per-channel WAV written). The editor wires
    // this once and, in the handler, fills any channel review whose WAV
    // filename was not yet known at review-creation time. A fixed wire, not
    // per-capture tracking state.
    std::function<void()> onCaptureSaveComplete;
    int getSnapshotCount() const;

    // Returns true once when auto-feedback is ready (consumed on read)
    bool shouldAutoFeedback() const { return autoFeedbackReady.exchange(false); }
    bool isAudioSilent() const { return audioSilent.load(); }
    bool isTransportPlaying() const { return transportPlaying.load(); }

    // Chat history — stored here so it persists when the editor is destroyed/recreated
    struct ChatEntry { 
        juce::String role, content; 
        bool hasWaveform = false;
        std::vector<float> waveform;  // simplified thumbnail (peak values)
        float durationSeconds = 0.0f;
        float lufs = -100.0f;
        juce::String wavFilename;
        juce::String wavFilePath;
    };
    std::vector<ChatEntry> chatHistory;
    // Editor-lifecycle survival (26 Jul bug): Logic recreates the plugin
    // editor on every Link<->EchoJay window switch, so editor-instance
    // state that must survive that boundary lives HERE, like chatHistory.
    // activeChatId lets a recreated editor re-hydrate the conversation
    // (full block state) from the workspace once its async load lands;
    // the chat target pill keeps pointing at the same rack. Message
    // thread only.
    juce::String activeChatId;
    juce::String chatTargetLinkUid;    // "" = local rack (compose target)
    juce::String chatTargetLinkName;   // display label when uid set
    // Phase C3: channel tapped but no chat record yet — creation is lazy
    // at FIRST SEND (a bare tap must not mint an empty record), so the
    // pending selection holds the channel here until then. Cleared when
    // any chat activates or the first send converts it into a real chat.
    juce::String pendingChannelUid;
    juce::StringArray chatRoles, chatContents; // for API context window

    // Visual mode state — persisted with DAW session
    int visualPreset = 0;   // 0=Orb, 1=Ring, 2=Helix, 3=Scatter
    int visualTheme = 1;    // 0=Nebula, 1=Aurora, 2=Solar, 3=Crystal
    bool visualModeOn = true;

    // CHAIN state — loaded plugin desc (serialised for DAW session restore)
    juce::String chainLoadedDescXml;
    bool chainWarningDismissed = false;

    // A/B playback — toggle between DAW audio and reference WAV
    void loadABFile(const juce::String& wavPath, double startOffsetSeconds = 0.0);
    void stopAB();
    void pauseAB();
    void resumeAB();
    std::atomic<bool> abActive { false };      // file is loaded
    std::atomic<bool> abPlayingRef { false };   // true = outputting ref WAV, false = DAW passthrough
    std::atomic<bool> abPaused { false };       // paused state — position held
    std::atomic<bool> abPausedByTransport { false }; // paused due to DAW stop (not user)
    std::atomic<bool> abSyncToDAW { false };    // sync AB position to DAW transport
    juce::AudioBuffer<float> abBuffer;
    int abPlaybackPos = 0;
    int abSampleCount = 0;
    double abSampleRate = 44100.0;
    juce::String abFilePath;
    mutable std::mutex abMutex;

    // ===== Compare dual-stream playback =====
    // Two independent streams: both advance + analyse simultaneously.
    // Only the audible one replaces the output buffer.
    struct CmpStream {
        juce::AudioBuffer<float> buffer;
        std::atomic<bool> loaded { false };   // file loaded into buffer
        std::atomic<bool> playing { false };  // actively advancing playback
        // Click-free monitor crossfade (25 Jul 2026, codec-preview safety):
        // monGain ramps toward (playing && audible) in processBlock (8ms).
        // stopAtZero = fade out then self-stop on the audio thread, used to
        // disengage codec preview without a click. monGain is audio-thread
        // only (reset under cmpMutex on load).
        std::atomic<bool> stopAtZero { false };
        float monGain = 0.0f;
        int playbackPos = 0;
        int sampleCount = 0;
        double sampleRate = 44100.0;
        juce::String filePath;
    };
    CmpStream cmpStream[2];
    std::atomic<uint32_t> audioBlocksProcessed_ { 0 };
    // SYNC position-follow (live A + reference B): reference slots track
    // the DAW playhead. ref time = host time + offset; offset is 0 on SYNC
    // enable and captured when the user click-seeks the reference during
    // sync (lining a drop up against a different arrangement).
    std::atomic<bool>   cmpSlotIsRef[2] { { false }, { false } };  // set by editor
    std::atomic<double> cmpSyncOffsetSec  { 0.0 };
    std::atomic<double> cmpLastHostTimeSec { -1.0 };  // last playhead seconds
    mutable std::mutex cmpMutex;             // protects both streams' buffers
    std::atomic<int> cmpAudible { -1 };      // which stream is audible (-1 = none)
    std::atomic<bool> cmpSyncToTransport { true }; // sync capture playback to DAW transport
    std::atomic<bool> cmpBothCaptures { false };   // true when both slots are captures (set by editor)
    // Temp buffers for muted-stream analysis (pre-allocated, avoids alloc on audio thread)
    juce::AudioBuffer<float> cmpTmpBuf;
    juce::AudioBuffer<float> cmpMixBuf;        // crossfade accumulation
    std::vector<float> cmpGainScratch;         // per-sample total monitor gain

    void loadCompareFile(int slot, const juce::String& wavPath);
    void fadeOutCompareStreams();   // ramp monitor gain to 0, streams self-stop (click-free)
    void stopCompareStream(int slot);
    void stopAllCompare();

private:
    MeterEngine meterEngine;       // Live meters (always running — live input only)
    MeterEngine captureEngine;     // Capture pass meters (reset each capture)
    MeterEngine abMeterEngine;     // AB playback spectrum only (used by Compare playing-slot panel)
    MeterEngine cmpMeter[2];       // Compare stream meters (one per slot)
    PluginScanner pluginScanner;
    ReferenceAnalyser refAnalyser;
    WaveformRecorder waveformRecorder; // Audio recording + waveform thumbnail
    ChainHost chainHost;           // Plugin chain hosting (CHAIN tab)

    ChannelType channelType { ChannelType::FullMix };
    juce::String customChannelName;
    bool channelTypePromptDismissed = false;
    juce::String genre { "hip-hop" };
    bool genrePromptDismissed = false;
    bool projectPromptDismissed = false;

    // Auto-detection
    
    
    // Spectrum accumulators during capture — both maintained simultaneously
    std::array<float, 64> spectrumPeak = {};   // peak-hold (for individual channels)
    std::array<float, 64> spectrumSum = {};     // running sum (for average on buses/mixes)
    int spectrumFrames = 0;
    
    // Per-capture aggregators for crest/RMS/peak/width/correlation.
    // Reset on capture start, updated per-buffer in audio thread, finalized on
    // capture stop. Using these instead of the meter engine's instantaneous
    // values (which is what was producing the wildly inconsistent snapshot data
    // — a snapshot was just "whatever the meter happened to read at the moment
    // Capture was clicked", not a measurement of the captured audio).
    //
    // Each is updated atomically per-buffer and read on the message thread
    // when stopCapture() runs. The values are simple summed accumulators rather
    // than running averages so we don't lose precision over long captures.
    std::atomic<float> capPeakL { 0.0f };       // max abs sample L over capture
    std::atomic<float> capPeakR { 0.0f };       // max abs sample R over capture
    std::atomic<double> capSumSqL { 0.0 };      // sum of x^2 over all samples L (for total RMS)
    std::atomic<double> capSumSqR { 0.0 };      // sum of x^2 over all samples R (for total RMS)
    std::atomic<double> capGatedSumSqL { 0.0 }; // sum of x^2 only over gated buffers L (for gated RMS)
    std::atomic<double> capGatedSumSqR { 0.0 }; // sum of x^2 only over gated buffers R (for gated RMS)
    std::atomic<long long> capTotalSamples { 0 };       // total samples processed
    std::atomic<long long> capGatedSamples { 0 };       // samples that passed the gate
    std::atomic<double> capWidthSum { 0.0 };    // sum of per-buffer width readings (gated)
    std::atomic<double> capCorrSum { 0.0 };     // sum of per-buffer correlation readings (gated)
    std::atomic<int> capGatedBufCount { 0 };    // number of buffers that passed the gate
    std::atomic<float> capRunningPeakForGate { 0.0f };  // running max peak, used as the moving gate threshold
    std::atomic<float> capMaxMomentary { -100.0f };     // highest momentary LUFS over capture
    std::atomic<float> capMaxShortTerm { -100.0f };     // highest short-term LUFS over capture

    // Capture
    std::atomic<CaptureState> captureState { CaptureState::Idle };
    juce::int64 captureStartTime = 0;
    int captureSampleCount = 0;
    mutable std::mutex snapshotMutex;
    std::vector<CaptureSnapshot> snapshots;
    int passCounter = 0;

    // Project naming — persisted in plugin state
    juce::String projectName;    // empty = use "Pass N" naming
    int captureVersion = 1;      // incremented after each capture when project is set; resets when name changes

    // Auto-feedback
    mutable std::atomic<bool> autoFeedbackReady { false };

    // Silence detection (triggers auto-stop when DAW stops)
    std::atomic<bool> audioSilent { true };
    std::atomic<bool> transportPlaying { false };
    bool wasTransportPlaying = false;
    int silenceCounter = 0;
    bool wasReceivingAudio = false;

    // Background WAV save thread — destructor waits for it to finish
    std::unique_ptr<juce::Thread> saveThread;

    // Background cache-load thread, launched from the constructor. Tracked
    // (not fire-and-forget) so the destructor can join it deterministically.
    // On full plugin removal the host destroys this processor; if this thread
    // were still inside pluginScanner.loadCache() while members tear down, the
    // result is a teardown race / freeze (observed on Windows/Cubase VST3,
    // where teardown timing differs from macOS). isShuttingDown lets the load
    // work bail early, and joining here guarantees it has stopped touching
    // members before they are destroyed.
    std::thread loadThread;
    std::atomic<bool> isShuttingDown { false };

    // =========================================================================
    //  EchoJay Link consumer — stage 2 (auto-discovery via registry)
    // =========================================================================
public:
    struct LinkSlotInfo {
        juce::String name;
        juce::String uid;                 // per-instance address ("" from old Links)
        bool         connected  = false;
        bool         active     = true;   // Link's capture/meter role (its Active toggle)
        float        sampleRate = 0.f;
        int64_t      framesRead = 0;
        int          regIdx     = -1;     // registry slot index (meter frame lookup)
        float        gainDb     = 0.0f;   // Link's built-in gain stage (0 = old Links)
        int          placement  = 0;      // 0 unset/unknown, 1 bus, 2 insert
    };

    /// Refresh the list of known Link slots from the registry.
    /// Call from the message thread (editor timer, ~2 Hz).
    void refreshLinkRegistry();

    /// Read a Link's latest published meter frame (message thread). Returns
    /// false on torn read / no registry — keep the previous copy. Staleness
    /// is detected by out.seq not advancing between reads (~10Hz expected).
    bool readLinkMeterFrame(int regIdx, LinkMeterFrame& out);

    /// Snapshot of currently known slots — message thread only.
    const std::vector<LinkSlotInfo>& getLinkSlotInfos() const { return linkSlotInfos; }

    /// One Link plus its Monitor-consistent display label. The label is a
    /// display string only ("Untitled", "Untitled 2", ...); the ADDRESS is
    /// always info.uid (never the name — duplicate "Untitled" names collide).
    struct LinkDisplayEntry {
        juce::String displayName;
        LinkSlotInfo info;
    };
    /// Canonical list every Link-listing surface must use so the whole product
    /// agrees on which Links exist and what they're called. Sorted named-first
    /// (alphabetical) then untitled (stable by uid); the "Untitled N" numbering
    /// is assigned over the FULL set so a given instance keeps the same label
    /// in the Monitor, the send-target menu and the AI context alike.
    std::vector<LinkDisplayEntry> getLinkDisplayList() const;
    // ONE accessor for a Link channel's display name (Phase N precedence via
    // getLinkDisplayList) — banner, dropdown, monitor, capture composition
    // and injections all resolve through THIS, keyed by the stable uid, so
    // a rename can never leave one surface on a frozen name.
    juce::String resolveLinkDisplayName(const juce::String& uid) const;

    // Consumer diagnostics (message thread, read by editor in paint)
    struct ConsumerDiag {
        juce::String regKey;
        bool         regOpened = false;
        int          regErrno  = 0;
        int          activeSlotCount = 0;
        juce::String nameList;    // comma-separated display names
    };
    ConsumerDiag consumerDiag;

    struct LinkCaptureChannel; // defined in PluginProcessor.cpp

private:
    static constexpr int kMaxLinkSlots = 16;

    // Resolved shared directory (message thread, set once in ensureLinkRegistryOpen)
    juce::String linkResolvedDir;

    // Registry mapping (message thread)
    void*  linkRegMap = nullptr;
    int    linkRegFd  = -1;

    void ensureLinkRegistryOpen();
    void closeLinkRegistryNow();   // destructor

    // Per-slot audio ring mappings — SpinLock for audio/message thread safety
    struct ActiveLinkSlot {
        juce::SpinLock       lock;
        void*                map    = nullptr;
        int                  fd     = -1;
        juce::String         shmKey;  // last opened key, message thread
        juce::String         displayName;  // message-thread only, set on connect
        juce::String         uid;      // Link instance uid — stable identity for
                                       // live name resolution (message thread)
        LinkShm::FileIdentity boundId; // dev+inode of the mapped ring; a path
                                       // now pointing elsewhere = stale ring
        std::atomic<int64_t> framesRead { 0 };
        // Non-copyable due to SpinLock — managed in-place via std::array
    };
    std::array<ActiveLinkSlot, kMaxLinkSlots> activeLinkSlots;

    void connectLinkAudioSlot   (int i, const juce::String& key, const juce::String& displayName,
                                 float sr, const juce::String& uid);
    void disconnectLinkAudioSlot(int i);
    void disconnectAllLinkSlotsNow();  // destructor

    // Stale detection (message thread)
    struct SlotProbeState { uint32_t lastHb = 0; int staleCycles = 0; };
    std::array<SlotProbeState, kMaxLinkSlots> slotProbeStates;

    // UI snapshot (message thread only)
    std::vector<LinkSlotInfo> linkSlotInfos;

    // Per-link capture channels — message thread writes, audio thread reads via spinlock
    std::vector<std::unique_ptr<LinkCaptureChannel>> linkCaptureChannels;
    juce::SpinLock                                   linkCaptureSpinLock;
    double hostSampleRate_    = 44100.0;
    int    hostSamplesPerBlock_ = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoJayProcessor)
};
