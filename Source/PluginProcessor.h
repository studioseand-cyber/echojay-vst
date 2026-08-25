#pragma once
#include <JuceHeader.h>
#include <thread>
#include <atomic>
#include "MeterEngine.h"
#include "PluginScanner.h"
#include "ReferenceAnalyser.h"
#include "WaveformRecorder.h"
#include "ChainHost.h"
#include "EchoJayAPI.h"
#include "DashPoll.h"
#include "LinkShm.h"
#include "EedKeyEngine.h"   // self-detection on music-bus roles (§6.1)
#include "EedKeyWorker.h"

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
    // Item 3: set when this capture was CHANNEL-scoped (the editor knew the
    // active channel). The snapshot still exists (the save thread writes its
    // channel WAV paths by index and the handshake reads them), but the
    // Compare list SKIPS it — the channel's own review is the entry, so
    // listing the host snapshot too read as "captured twice". Empty = a
    // normal full-scope session capture, listed as usual.
    juce::String channelScopeUid;
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

    // Detected key (KEY_PRECONDITION_SPEC.md §5.2): an OFFLINE pass run by the
    // save thread when the capture is made — longer window, HPSS + Viterbi
    // over up to three windows of the stored audio, the highest-quality key
    // source in the product. Stored with the capture so it travels with the
    // material instead of being re-derived. keyValid=false means the pass was
    // not run (no trustworthy source — see the source rule in stopCapture) or
    // found nothing tonal; that absence is itself the honest answer. Age is
    // derived from `timestamp`. keySourceName says WHICH channel was read
    // ("Music Bus" / "Mix Bus"); keySourcePlacement 1 = a bus Link channel,
    // 0 = the host channel.
    bool  keyValid        = false;
    int   keyRoot         = 0;      // 0..11 C..B
    bool  keyMinor        = false;
    float keyConfidence   = 0.0f;   // 0..1
    float keyTuningHz     = 0.0f;   // detected reference pitch
    float keyTuningCents  = 0.0f;   // same offset in cents from A=440
    std::array<float, 12> keyChroma {};   // for the Meters wheel
    int   keyAltRoot      = -1;     // best alternate (-1 = none)
    bool  keyAltMinor     = false;
    float keyAltScore     = 0.0f;
    juce::String keySourceName;
    int   keySourcePlacement = 0;

    juce::String getChannelDisplayName() const {
        if (channelType == ChannelType::Other && customChannelName.isNotEmpty())
            return customChannelName;
        return channelTypeNames[(int)channelType];
    }
};

class EchoJayProcessor : public juce::AudioProcessor,
                         private juce::Timer   // §6.1 self-key scheduler (1 Hz)
{
public:
    EchoJayProcessor();
    ~EchoJayProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    // The host's name for this track (Logic's context name, VST3 channel
    // context). Read for the running level tally's source guard: a name
    // CHANGE resets the tally, and the saved tally is discarded on restore
    // when the names differ (ChainHost::setPendingLevelsState). Any thread
    // (the AU property listener fires off the message thread): stash + flag,
    // applied by the 1 Hz timer on the message thread, exactly as the Link
    // does it (LinkProcessor::updateTrackProperties).
    void updateTrackProperties(const TrackProperties& props) override;
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

    // ---- Self key detection (KEY_PRECONDITION_SPEC.md §6.1) --------------
    // When THIS plugin's declared channel role IS the music (Mix Bus /
    // Master / Music / Instrument Bus), it detects its own channel's key
    // passively — same engine, same duty cycle and gating as a Link (§5.1),
    // and the reading ranks as a BUS reading. The 5.3 rule restated: the
    // disqualifier was never "the channel EchoJay is on", it is "a channel
    // that is not the music", judged by DECLARED ROLE — so a vocal or
    // unknown role never self-detects as a primary source.
    static bool isMusicBusRole(ChannelType t)
    {
        return t == ChannelType::FullMix || t == ChannelType::MasterBus
            || t == ChannelType::MusicBus || t == ChannelType::InstrumentBus;
    }
    bool selfKeyRoleIsMusic() const { return isMusicBusRole(channelType); }
    echojay::KeyEngine& getSelfKeyEngine() { return selfKeyEngine_; }
    // Millisecond stamp of the last published change (0 = never) — the age.
    juce::uint32 selfKeyChangeMs() const { return selfKeyWorker_.lastChangeMs(); }
    // RE-ANALYSE on the self reading: arm a committed pass NOW.
    void armSelfKeyAnalysis();

    // ---- Key source pin (KEY_PRECONDITION_SPEC.md §7) --------------------
    // "" = Auto (precedence decides, today's behaviour). Otherwise a stable
    // id: "self" | "chain" | "capture" | "link:<uid>". The label is the
    // display name AT PIN TIME, kept so a pinned source that later
    // disappears can be NAMED in the "gone — showing Auto" message instead
    // of silently swapped. Persisted per instance. Pinning "self" force-runs
    // the self engine even on a non-music role (§7.2: an explicit choice
    // beats the declared-role inference — the likeliest reason to pin a
    // "vocal" channel is that the role is mis-declared).
    juce::String getKeySourcePin()      const { return keySourcePin_; }
    juce::String getKeySourcePinLabel() const { return keySourcePinLabel_; }
    void setKeySourcePin(const juce::String& pinId, const juce::String& label);

    // ---- key source collection (KEY_DETECTOR_SPEC §9, PITCH spec §6) -----
    // THE ONE precedence walk that ranks every key source — capture, bus
    // Link, self-on-music-bus, channel Link, local chain — lives HERE, on
    // the processor, and the editor delegates to it. It lived on the editor
    // first, and that placement was itself a bug: the KeyFeed that EchoJay
    // Pitch follows was only published from the editor's timer, so closing
    // the plugin window froze the key at its last value — the stale-key
    // failure the chromatic fallback exists to prevent, arriving by a
    // different route. Collection now runs (and publishes) on the
    // processor's own 1 Hz timer for the life of the instance; the editor
    // merely reads the same walk for its UI.
    struct KeySourceReading
    {
        enum class Kind { Capture, BusLink, SelfBus, ChannelLink, LocalChain };
        Kind  kind = Kind::LocalChain;
        juce::String name, uid;        // uid: Link instance uid (Links only)
        juce::String detail;           // capture: which channel the offline
                                       // pass read ("Music Bus (bus Link)")
        int   placement = 0;           // registry value (Links only)
        int   root = 0; bool minor = false;
        float conf = 0.0f, tuningHz = 440.0f, tuningCents = 0.0f, rootHz = 0.0f;
        juce::uint32 ageMs = 0;
        bool  committed = false;
        float analysedSeconds = 0.0f;
        bool  hasChroma = false;
        std::array<float, 12> chroma {};
        int   altRoot = -1; bool altMinor = false; float altScore = 0.0f;
        bool  poisoned = false;        // vocal-channel local reading: never use
        // §7 source selector: sources that EXIST but have no reading yet
        // still appear in the menu (hasReading=false, skipped by precedence);
        // unusableReason non-empty = greyed in the menu WITH the why, still
        // pinnable. pinId is the stable identity the pin persists.
        bool  hasReading = true;
        juce::String pinId;            // "self" | "chain" | "capture" | "link:<uid>"
        juce::String unusableReason;
    };
    struct KeySources
    {
        std::vector<KeySourceReading> all;   // precedence order (menu order)
        int  primaryIdx = -1;                // -1 = nothing usable
        bool disagree   = false;             // usable sources name different keys
        // §7: autoIdx is what precedence picks IGNORING the pin (so the menu
        // can always show what Auto resolves to); primaryIdx is after the
        // pin is applied. A pin that resolves to a source with a reading
        // sets userSelected; a pin whose source is gone sets pinMissing —
        // stated, never silently replaced.
        int  autoIdx      = -1;
        int  pinnedIdx    = -1;
        bool userSelected = false;
        bool pinMissing   = false;
        juce::String pinMissingLabel;
        const KeySourceReading* primary() const
        { return primaryIdx >= 0 ? &all[(size_t) primaryIdx] : nullptr; }
    };
    static constexpr juce::uint32 kCaptureKeyFreshMs = 15 * 60 * 1000;
    KeySources collectKeySources();
    // Build the resolved-primary fact and publish it to echojay::KeyFeed.
    // Message thread only (the 1 Hz timer, and the editor's 2 Hz refresh —
    // publishing from both is harmless: same walk, same fact).
    void publishKeyFeed(const KeySources& sources);

    MeterEngine& getMeterEngine()   { return meterEngine; }
    MeterEngine& getABMeterEngine() { return abMeterEngine; }
    MeterEngine& getCompareMeter(int slot) { return (slot == 0) ? cmpMeter[0] : cmpMeter[1]; }
    PluginScanner& getPluginScanner() { return pluginScanner; }
    ReferenceAnalyser& getReferenceAnalyser() { return refAnalyser; }
    WaveformRecorder& getWaveformRecorder() { return waveformRecorder; }
    ChainHost& getChainHost() { return chainHost; }

    /** THE ONE API CLIENT, moved here from the editor for Session C.
     *
     *  WHY IT LIVES ON THE PROCESSOR. Logic destroys and recreates the plugin
     *  editor on every Link window switch, which in real Link work happens
     *  every couple of minutes. Session C's 20 second community poll has to
     *  survive that, so the poll timer must live on something that outlives
     *  the editor, and the timer needs an API client. Same rule as
     *  chatHistory, applied to the network layer.
     *
     *  WHY NOT A SECOND CLIENT ON THE PROCESSOR. ~EchoJayAPI() calls
     *  saveSettings(). Two instances would both write the same settings file
     *  on teardown and the last one destroyed would win, which is
     *  last-writer-wins corruption that surfaces weeks later as a mysterious
     *  logout. The token is also loaded from disk at construction, so a login
     *  through one instance would leave the other holding a stale token and
     *  silently 401ing. Sharing one client is not the smaller change, it is
     *  the correct one.
     *
     *  LIFETIME, which is the real risk and is guaranteed rather than hoped:
     *  JUCE's AudioProcessor OWNS its editor. AudioProcessorEditor's
     *  constructor takes the processor, and the host deletes the editor
     *  before the processor via AudioProcessor::editorBeingDeleted, which
     *  JUCE calls from ~AudioProcessorEditor. A processor is never destroyed
     *  with a live editor attached, and this member is declared as a plain
     *  value on the processor, so it is destroyed in ~EchoJayProcessor AFTER
     *  the editor has already gone. The reference the editor holds therefore
     *  cannot outlive the referent.
     *
     *  Two behaviour changes that come with the move, both improvements and
     *  both deliberate: the constructor's fetchRemoteConfig and
     *  refreshUserInfo now fire once per plugin instance rather than once per
     *  editor recreation, and saveSettings runs once per processor teardown
     *  rather than once per editor teardown. The one thing that was NOT an
     *  improvement, per-turn chat staging surviving an editor swap, is
     *  handled by clearStagedTurn(); see the note on it in EchoJayAPI.h.
     */
    EchoJayAPI& getApi() { return api; }

    // Save captured audio to WAV in the project/capture folder
    juce::String saveCaptureWAV();
    juce::File getCaptureFolder() const;
    
    // Build AI compare context between a capture and a reference
    juce::String buildCompareContext(const CaptureSnapshot& capture, const ReferenceResult& reference) const;
    // Build AI compare context between two captures
    juce::String buildCompareContext(const CaptureSnapshot& a, const CaptureSnapshot& b) const;
    // Build AI compare context between two references
    juce::String buildCompareContext(const ReferenceResult& a, const ReferenceResult& b) const;
    // Slot-unified compare context (from getSlotMeterData): the ONE source
    // AI Compare now reads, so the gate, the comparison, the suppression and
    // the labels cannot disagree. numbersOnly: cross-scope "Numbers only"
    // choice - meter figures only, DIFFERENT-SOURCES statement, no chain.
    juce::String buildCompareContext(const MeterData& a, const MeterData& b,
                                     const juce::String& labelA, const juce::String& labelB,
                                     float durA, float durB, bool numbersOnly) const;
    // Figure-CARD data (client-rendered at compose time): both sources' figures
    // + labels + a cross-scope flag, enough to redraw the card identically on
    // reload. Only-present keys; absent = N/A. Shares computeCompareFig with the
    // text table so the card can never disagree with the model's numbers.
    juce::String buildCompareFiguresJson(const MeterData& a, const MeterData& b,
                                         const juce::String& labelA, const juce::String& labelB,
                                         bool crossScope) const;

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

    // =====================================================================
    // Stage 1 remote editing: the SOLO session.
    //
    // ON THE PROCESSOR, every part of it, because Logic recreates the editor
    // on every Link-window switch: an editor-held session (or lease) would
    // release mid-edit every couple of minutes. The editor initiates and
    // reports; the processor owns the instance, the lease renewals and the
    // audio path.
    //
    // AUDIO CONTRACT: processBlock takes editLock_ with tryEnter ONLY (a
    // miss = the mix passes through unchanged for one block); the message
    // thread takes it blocking, briefly, to swap the instance in or out.
    // The instance is prepared BEFORE it is installed and destroyed a
    // deferred beat AFTER it is removed, so the audio thread never sees a
    // half-ready plugin and never runs one being torn down.
    // =====================================================================
    struct EditSession {
        juce::String uid;              // the Link (message thread)
        int          slot0 = -1;       // its rack slot (message thread)
        juce::String leaseId;          // this session's identity
        juce::String pluginName, pluginFormat;
        juce::String keptStateB64;     // preserved on teardown: edits not lost
        juce::String keptUid; int keptSlot0 = -1;   // what the kept state is FOR
        uint32_t     beganMs = 0;      // grace window for lease-death watch
        std::atomic<int>  ringSlot { -1 };   // activeLinkSlots index, -1 = none
        std::atomic<bool> audioOn  { false };
    };
    EditSession editSession_;
    juce::SpinLock editLock_;
    // AudioProcessor, NOT AudioPluginInstance, and the widening is what lets a
    // BUILT-IN device be edited: the registry factory hands back a plain
    // AudioProcessor (an EedDeviceProcessor), never an AudioPluginInstance,
    // so the narrower type excluded every internal device by construction.
    // Nothing the edit path calls needs the derived type: prepareToPlay,
    // processBlock, get/setStateInformation, createEditor and
    // releaseResources are all declared on AudioProcessor.
    std::unique_ptr<juce::AudioProcessor> editInst_;        // under editLock_
    juce::AudioBuffer<float> editBuf_;                      // audio scratch
    juce::LinearSmoothedValue<float> editSoloMix_;          // 0 mix .. 1 solo
    // Ring cushion policy for the solo consumer. Engage seeks to the target;
    // per block, a backlog past the trip re-seeks to the target. 1024 frames
    // is ~23ms at 44.1k: enough cushion that scheduling jitter does not
    // starve a block, small enough that the audition feels live. Solo aligns
    // against nothing (nothing else is audible), so this bounds LATENCY, not
    // alignment; stage 2's stamps do alignment.
    static constexpr uint32_t kEditCushionFrames = 1024;
    // §8.3 (amended): the FIXED alignment budget, reported from
    // instantiation so ordinary browsing never re-runs PDC. Measured, not
    // guessed (this machine's catalogue, defaults, 48k): worst single slot
    // found = Ozone 12 Low End Focus at 12,799; the common latency class
    // sits at or under ~4k. 16384 = cushion 1024 + headroom 15360.
    static constexpr int kBorrowAlignBudgetFrames = 16384;
    // The pad that keeps every mode's TOTAL at exactly the budget:
    // pad = budget - cushion - borrowedChainLatency; negative = the rack
    // does not fit — in-context REFUSED for it (solo fallback, named line).
    // Pure and public so the arithmetic is functionally gated.
    static int alignPad(int borrowedChainLatency) noexcept
    {
        const int pad = kBorrowAlignBudgetFrames
                      - (int) kEditCushionFrames - borrowedChainLatency;
        return pad >= 0 ? pad : -1;
    }
    // Two integer-frame ring delays realise the split (pre-sum alignment on
    // the passthrough; final pad after the main chain). Fixed capacity, no
    // allocation on the audio thread.
    struct AlignDelay
    {
        juce::AudioBuffer<float> buf; int w = 0;
        void prepare(int cap) { buf.setSize(2, cap); buf.clear(); w = 0; }
        void process(juce::AudioBuffer<float>& b, int delay)
        {
            const int cap = buf.getNumSamples();
            if (cap == 0 || delay <= 0) return;
            const int n = b.getNumSamples();
            for (int i = 0; i < n; ++i)
            {
                const int r = (w - delay % cap + cap) % cap;
                for (int ch = 0; ch < 2 && ch < b.getNumChannels(); ++ch)
                {
                    float* d = b.getWritePointer(ch);
                    const float in = d[i];
                    buf.setSample(ch, w, in);
                    d[i] = buf.getSample(ch, r);
                }
                w = (w + 1) % cap;
            }
        }
    };
    static constexpr uint32_t kEditReseekTrip    = 8192;

    bool editActive() const { return editSession_.slot0 >= 0; }
    /// Install a prepared instance and start the lease (message thread).
    void editBegin(const juce::String& uid, int slot0,
                   const juce::String& name, const juce::String& fmt,
                   std::unique_ptr<juce::AudioProcessor> inst,
                   const juce::String& leaseId);
    /// Tear down. keepState captures the instance's state into keptStateB64
    /// first (the lease-died path: nothing the user did is lost).
    void editEnd(bool keepState);
    /// Current edited state as base64, for the commit (message thread).
    juce::String editCaptureStateB64();
    juce::AudioProcessorEditor* editCreateEditor();
    void renewEditLease();          // the 1s writer
    struct EditLeaseTimer;
    std::unique_ptr<EditLeaseTimer> editLeaseTimer_;

    // ---- Whole-rack borrow, step 2 (RACK_BORROW_IMPLEMENTATION_SPEC §3/§4)
    // Solo only, NO COMMIT: edits are audible and uncommitted; nothing in
    // this step writes state back to the Link (step 3 owns Apply). The
    // borrowed host is session-long (spec §1); the session engages/releases.
    ChainHost*   borrowHost();                       // lazy, Mode::Borrowed
    ChainHost*   borrowHostIfActiveFor(const juce::String& uid);
    bool         borrowActive() const noexcept
    { return borrowSession_.active.load(std::memory_order_relaxed); }
    juce::String borrowUid() const { return borrowSession_.uid; }
    // Engage half A (message thread): create/prepare the host, bind the ring,
    // start the rack-scoped lease renew. The EDITOR builds the rack (pulls +
    // restore) before calling borrowAudioOn().
    // structureCapable: the engage-time snapshot, set HERE so it rides the
    // session from its first instant — never deferred to load settlement.
    void borrowEngageBegin(const juce::String& uid, const juce::String& leaseId,
                           bool structureCapable = false,
                           bool inContextCapable = false);
    // borrowAudioOn/Off/IsOn (LISTEN) DELETED 27 Aug 2026 — solo subsumes
    // LISTEN (MUTE_SOLO_SPEC). The fallback solo is automatic (!inContextOk).
    // Release: audio off (ramped), lease file deleted (Link restores all
    // bypasses through its one restore path), instances parked in the pool.
    // keepEdits=true (auto-release, editor/processor teardown) captures every
    // slot's current state into borrowKept_ FIRST — the continuous-keep
    // promise: a crash or lease death loses nothing. User Apply/Discard pass
    // false: the edits were committed, or the user explicitly chose loss.
    void borrowRelease(bool keepEdits = false);

    // ---- §5a-R (26 Aug 2026): selection IS the session ---------------------
    // The apply orchestration lives on the PROCESSOR because an editor being
    // destroyed cannot poll an ack (ruling 4: editor close applies, then
    // releases, and never strands a lock). Verdicts and the plan build move
    // here with it — they only ever read processor state.
    std::vector<std::pair<bool, bool>> borrowSlotVerdicts();  // {withheld, edited}
    LinkShm::StructureEdit::Plan buildStructurePlan();
    bool borrowSessionShapeDirty() const;
    // Compute the plan ONCE and send it; on success release + sticky banner
    // + revert offer. releaseLockOnFail: false = deselect semantics (the
    // session stays engaged, lock holds, says why); true = editor-close
    // semantics (lock releases regardless — no lock without a visible
    // owner — edits kept, unwritten-note recorded, failure LOGGED whether
    // or not a window ever reopens).
    void borrowApplyAndRelease(bool releaseLockOnFail);
    void borrowEditorClosed();
    // §3f pin, restored in §5a-R terms (26 Aug 2026 ping-pong): while a
    // session is LIVE its uid is authoritative — a chat activation may
    // move the VIEW, never the session. Only a USER-initiated selection
    // change commits (a deselect caused by something grabbing the
    // selection is not the user saying "write this"). Pure, so every arm
    // is functionally gated; the editor routes through it.
    enum class SelDecision { Nothing, ViewOnly, ApplyAndPend, PendEngage };
    static SelDecision decideSelection(bool sessionActive, bool sameUid,
                                       bool userInitiated) noexcept
    {
        if (sessionActive && sameUid)   return SelDecision::Nothing;
        if (sessionActive)              return userInitiated
                                            ? SelDecision::ApplyAndPend
                                            : SelDecision::ViewOnly;
        return userInitiated ? SelDecision::PendEngage
                             : SelDecision::ViewOnly;
    }

    // The slot-editor decision, ONE author, FUNCTIONALLY gated (twice
    // regressed as editor-side code: 22 Aug guard order, 26 Aug an engage
    // that never completed — both survived source pins because a pin proves
    // a branch exists, not that it is reached). Order is load-bearing: the
    // borrowed arm must precede the remote guard, whose viewUid is
    // non-empty for a borrowed view too.
    juce::AudioProcessorEditor* createSlotEditorForView(
        const juce::String& viewUid, int slot);
    // Session-scoped surfaces, rendered by the panel until superseded —
    // banners must not be losable by navigating away (§5a-R honesty rule).
    juce::String borrowStickyBanner_;
    std::map<juce::String, juce::String> unwrittenEditNote_;  // uid -> note
    // A deselect-apply FAILED and ruling 3 kept the session engaged: the
    // hold is now a pending edit, not a selection. Rides the lease
    // (additive "editPending") so the Link's lock banner can say so
    // instead of instructing a deselect the user already performed.
    bool borrowEditPendingHeld_ = false;
    juce::String pendingAutoEngage_;   // engage this uid once released
    // §8 in-context state (public: the editor banners from it, the gates
    // assert it): OK = announced AND fits the budget, decided at engage,
    // re-checked live on every borrowed-chain change.
    std::atomic<bool> borrowInContextOk_ { false };
    std::atomic<int>  borrowChainLat_ { 0 };
    int borrowMuteUnconfirmedTicks_ = 0;   // §8 closed-loop watchdog
    int borrowLastPadKey_ = -2;            // §8 injection pad-change detector
    // §8.3 refinement (26 Aug 2026 ruling): the budget is carried ONLY when
    // the project has a capable Link. No Links -> no alignment budget -> no
    // added latency. The transition re-runs PDC once, on the deliberate and
    // rare act of adding/removing a Link — never on rack browsing. ONE
    // writer: the registry pass, via setBorrowBudgetActive.
    std::atomic<bool> borrowBudgetActive_ { false };
    void setBorrowBudgetActive(bool active);
    int  reportedBudgetFrames() const noexcept
    { return borrowBudgetActive_.load(std::memory_order_relaxed)
                 ? kBorrowAlignBudgetFrames : 0; }
    bool borrowApplyInFlight_ = false;

    // ---- Mute/solo layer (27 Aug 2026, MUTE_SOLO_SPEC) -------------------
    // Per-Link snapshot of the published mute/solo bits, refreshed by the
    // registry pass (mtime-gated parses — steady state costs no IO) from
    // LIVE rows only. Message thread; the editor's lamps and capability
    // gating read it directly.
    struct LinkMuteSoloSnap { bool muteUser = false, soloOn = false,
                              capable = false; juce::int64 mtimeMs = -1; };
    std::map<juce::String, LinkMuteSoloSnap> muteSoloSnaps_;
    bool soloSetActive_ = false;      // any LIVE row publishes soloOn
    int  soloIncapableLive_ = 0;      // live rows lacking muteSoloCapable
    // THE HONORARY STRIP (§6.1): the injection follows the fabric as if it
    // were the edited rack's strip — suppressed iff a solo set exists and
    // the edited rack is not in it. ONE writer (the registry pass); the
    // audio thread only reads.
    std::atomic<bool> borrowSoloSuppressInj_ { false };
    bool soloSuppressPrev_ = false;   // banner-transition detector
    // The injection gain this instant (the honorary strip's lamp) — public
    // so the gate asserts the BEHAVIOUR, not just the branch.
    float borrowCtxMixNow() const noexcept
    { return borrowCtxMix_.getCurrentValue(); }

    // ---- Step 3: Apply & Release bookkeeping (message thread only) --------
    // Per-slot record from engage: the saved identity triplet (the SAME
    // fields stateFitsPlugin withheld the pull by — Apply re-runs the same
    // verdict) and the post-seed BASELINE state, so "edited" means "byte-
    // differs from what the Link sounds like", not "differs from a pull that
    // may have been empty".
    struct BorrowSlotRecord {
        juce::String name, savedFormat, savedVersion, savedUid, baselineB64;
        bool hadState = false;   // a state was pulled for this slot
    };
    std::vector<BorrowSlotRecord> borrowSlotRecords_;
    // Uncommitted edits captured at a keep-release; re-borrowing the same
    // uid restores them (and says so). Cleared by Apply, Discard, or the
    // restore itself.
    struct BorrowKept { juce::String uid; juce::StringArray names, states; };
    BorrowKept borrowKept_;
    void captureBorrowKept();
    void clearBorrowKept() { borrowKept_ = {}; }

    // ---- Phase 3: structure-edit session state (message thread only) ------
    // Per CURRENT borrowed slot: which base slot it came from (-1 = created
    // in the main). The records stay base-indexed and immutable; this map is
    // what reorders/removes/adds mutate, and what the plan reads.
    std::vector<int> borrowSlotOrigin_;
    // Created slots' identity, keyed by their position in borrowSlotOrigin_
    // being -1: name + decimal uid captured at add time.
    std::vector<LinkShm::StructureEdit::SlotIdentity> borrowCreatedIdentity_;
    // The base identity snapshot from engage (the plan guard's truth).
    std::vector<LinkShm::StructureEdit::SlotIdentity> borrowBaseIdentity_;
    // Capability snapshot at engage: structure ops offered only against a
    // Link that announced structureEditCapable THEN — never re-read mid-
    // session, so an old Link keeps settings-only behaviour throughout.
    bool borrowStructureCapable_ = false;
    // Removed-withheld memory: names of removed slots whose settings never
    // arrived — the confirm gives these their own line (spec: deleting
    // settings the user never saw). Checked AT removal (the node's seeded
    // fact dies with the slot).
    juce::StringArray borrowRemovedWithheld_;
    // Removed base names for the confirm's Removing line.
    juce::StringArray borrowRemovedNames_;
    void renewBorrowLease();                         // scope:"rack", slot:0
    void borrowTick();          // renew + ring re-bind (the 1s timer's body)
    /** Where the borrowed solo lands: through the main's own chain (Mix/
        Master Bus default) or replacing the output after it (every other
        channel type). The flip is the visible override; reset at engage. */
    bool borrowRouteThroughMain() const
    {
        return LinkShm::BorrowRoute::throughMainChain(
            channelType == ChannelType::FullMix || channelType == ChannelType::MasterBus,
            /*flip*/ false);   // the override retired with LISTEN — auto only
    }
    /** Set when the borrow released ITSELF (ring lost past tolerance) —
        consumed once by whichever editor next ticks, shown in words. A
        self-release must never be silent. */
    juce::String takeBorrowAutoReleaseReason()
    { return std::exchange(borrowAutoReleaseReason_, juce::String()); }
    struct BorrowSession {
        juce::String uid, leaseId;
        std::atomic<bool> active   { false };
        // audioOn (LISTEN) deleted 27 Aug 2026 — MUTE_SOLO_SPEC §6.4: the
        // fallback solo derives from !borrowInContextOk_, no user flag.
        std::atomic<int>  ringSlot { -1 };
    };
    BorrowSession borrowSession_;
private:
    std::unique_ptr<ChainHost> borrowHost_;
    juce::AudioBuffer<float>   borrowBuf_;
    juce::SpinLock             borrowLock_;
    juce::LinearSmoothedValue<float> borrowSoloMix_ { 0.0f };
    struct BorrowLeaseTimer;
    std::unique_ptr<BorrowLeaseTimer> borrowLeaseTimer_;
    int          borrowRingLostTicks_ = 0;           // message thread only
    juce::String borrowAutoReleaseReason_;           // message thread only
    std::atomic<bool> borrowRouteFlip_ { false };    // the visible override
    void applyBorrowSoloMixOn(juce::AudioBuffer<float>& buffer, bool on);
public:

    // ---- Rack lock (21 Aug 2026, RACK_BORROW_REQUIREMENTS §4) -------------
    // PROCESSOR renews, EDITOR gates: the editor declares which rack its
    // Chain tab is actively showing (empty = none), this class writes/renews
    // racklock-<uid>.json at 1s while it holds, deletes it the moment the
    // declaration clears, and lets the 3s expiry cover a crash. UI-only:
    // nothing here touches audio, bypass or the Active toggle. All state is
    // message-thread only, like the edit-session lease around it.
    enum class RackLockState { Idle, Held, WaitRecency, HeldByOther };
    void setRackLockWant(const juce::String& uid);   // editor's declaration
    RackLockState rackLockState() const { return rackLockState_; }
    juce::String  rackLockOtherOwner() const { return rackLockOtherOwner_; }
    bool rackLockHeldFor(const juce::String& uid) const
    { return rackLockState_ == RackLockState::Held && rackLockHeldUid_ == uid; }
    void rackLockTick();            // the 1s state machine (public for the timer)
private:
    struct RackLockTimer;
    std::unique_ptr<RackLockTimer> rackLockTimer_;
    juce::String  rackLockWantUid_, rackLockHeldUid_, rackLockOtherOwner_;
    juce::String  rackLockId_;      // stable identity for FCFS, minted on first use
    RackLockState rackLockState_ = RackLockState::Idle;
    juce::String  rackLockMyName() const;
    void          rackLockReleaseFile();
public:
    void startCapture();
    void stopCapture();
    void resetCapture();
    float getCaptureDuration() const;

    std::vector<CaptureSnapshot> getSnapshots() const;
    void renameSnapshot(int index, const juce::String& newName);
    void deleteSnapshot(int index);
    CaptureSnapshot getLatestSnapshot() const;
    // Item 1: the editor stamps the next capture's NAME and channel SCOPE at
    // Capture-PRESS time (both derive from the single chat-revision source it
    // owns). Stashed here and consumed when stopCapture pushes the snapshot,
    // so an editor recreate between press and completion cannot lose them
    // (the after-completion marker did have that window). Name empty = fall
    // back to computePassName(); scope empty = a full capture.
    void setNextCapture(const juce::String& name, const juce::String& scopeUid);
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

    /** Which tab the editor was last on, as the raw enum index.
        (0 = Dashboard; the enum itself lives in PluginEditor.h, which this
        header must not include.)

        HERE, not on the editor, for the standing reason: Logic destroys and
        recreates the editor every time the user switches between the Link
        window and EchoJay, several times a minute in real Link work. An editor
        member would bounce them back to the default tab on every switch.

        Deliberately NOT persisted into the plugin state blob. It answers "put
        me back where I was in this session", and a fresh instance starting at
        0 is what makes Dashboard the default on first launch after update. */
    int lastTabIndex = 0;

    // ===== Session C: the community poll ==================================
    //
    // The poller is PROCESS-WIDE and shared by every EchoJay instance in the
    // host, because the unread counts are per ACCOUNT and every instance in a
    // process shares one account and one settings file. Eight inserts used to
    // mean eight identical requests per tick; now it is one. See DashPoll.h.
    //
    // It is still NOT on the editor, which is the older and more important
    // rule: Logic destroys the editor on every Link window switch, so a timer
    // there restarts its interval constantly and the counts reset.
    //
    // This processor is a client of the shared poller: it lends its api for
    // requests and forwards change notifications to its editor.
    using DashUnread = DashPollShared::Unread;

    /** Current unread counts, from the shared poller. Message thread only. */
    const DashUnread& getDashUnread() const noexcept { return dashPoll->getUnread(); }

    /** Bumped only when the counts change. The editor compares this against
        what it last drew rather than diffing four fields. */
    int getDashUnreadGeneration() const noexcept     { return dashPoll->getGeneration(); }

    /** Fired on the message thread when the counts change. The editor sets
        this in its constructor and CLEARS IT FIRST in its destructor: a stale
        std::function holding a dangling editor is exactly the crash the
        editor-recreation cycle would produce twice a minute. */
    std::function<void()> onDashUnreadChanged;


    // Visual mode state — persisted with DAW session
    int visualPreset = 0;   // 0=Orb, 1=Ring, 2=Helix, 3=Scatter
    int visualTheme = 1;    // 0=Nebula, 1=Aurora, 2=Solar, 3=Crystal
    bool visualModeOn = true;

    // CHAIN state — loaded plugin desc (serialised for DAW session restore)
    juce::String chainLoadedDescXml;

    // ---- Saved chain identity (Session B.1) ------------------------------
    // Which saved chain the rack currently came from, so Save is never
    // ambiguous about what it overwrites and the name can be shown
    // persistently.
    //
    // On the PROCESSOR, like chatTargetLinkUid, because Logic recreates the
    // editor every time the user switches between the Link window and
    // EchoJay. Editor state would silently turn Save into Save As on a
    // window switch, which is the worst kind of wrong: it looks like it
    // worked and quietly makes a second chain.
    //
    // Persisted in plugin state, so reopening a project still knows which
    // saved chain the rack came from. Empty = nothing loaded, Save behaves
    // as Save As.
    juce::String savedChainId;
    juce::String savedChainName;

    // Chain sidebar mode: false = AI assistant, true = saved-chain browser.
    // On the processor for the same reason as the identity above: Logic
    // recreates the editor on every Link window switch, and a mode that
    // silently reverts mid-task is the kind of small wrongness that erodes
    // trust in everything else. NOT persisted to plugin state: which pane
    // you last looked at is not worth writing into a project file.
    bool chainSidebarChainsMode = false;
    bool chainWarningDismissed = false;

    /** Chat sidebar collapsed to zero width. ONE FLAG FOR EVERY SURFACE, not
        one per tab: collapse it on Link and it is collapsed on Compare too.
        Per-tab state would widen this to a lookup keyed by tab, and that is
        deliberately NOT pre-built.

        ON THE PROCESSOR, and this fixes a live bug rather than merely
        avoiding one. It used to be EchoJayEditor::chainChatCollapsed_, an
        editor member, so collapsing the Chain sidebar and switching to the
        Link window and back silently reopened it, several times a minute in
        real Link work. Logic destroys the editor on that switch; anything
        that must survive it lives here, like chatTargetLinkUid and
        savedChainId.

        PERSISTED, unlike chainSidebarChainsMode above, because this one is a
        layout choice about how you want the window rather than which pane you
        last looked at. Read back with a hasProperty guard, so a project saved
        before this existed loads expanded exactly as it does today and the
        migration is silent. */
    bool chatSidebarCollapsed = false;

    /** BUS OUTPUT TRIM (the mixer's Mix Bus fader). A plain persisted value,
        deliberately NOT a host-automatable parameter: this is a mixer trim
        on an analysis plugin, and an automatable output gain invites
        automating the very thing the measurements are calibrated against
        (and would change the published AU parameter list besides). Sits
        POST-CHAIN, PRE-METER-TAP in processBlock, the Link's placement for
        the Link's two reasons: the DAW hears it, and the meters read
        post-gain so level-match stays displayed == applied. Smoothed on the
        audio thread (LinearSmoothedValue, the Link's applyGainSmoothed shape
        verbatim), bit-transparent at 0 dB via the settled-at-unity early
        out. Clamped to the fader's own range. */
    static constexpr float kBusGainMinDb = -24.0f, kBusGainMaxDb = 12.0f;
    void  setBusGainDb(float db, bool snapSmoothing = false);
    float getBusGainDb() const { return busGainDb_.load(std::memory_order_relaxed); }

    /** LINK MIXER view controls. Here, not on the editor, for the same reason
        as chatSidebarCollapsed: Logic destroys the editor every time you
        switch between the Link window and EchoJay, which in real Link work is
        every couple of minutes. A view mode that resets on that switch is
        worse than no view mode, because the reset is invisible until you
        notice the strips changed under you.

        Persisted with a hasProperty guard, so a project saved before the
        mixer existed opens in the defaults below and the migration is silent.
        A persisted value wins on load.

        Selection is deliberately NOT here: the selected channel already
        derives from effectiveChannelUid() (the active chat's linkUid, else
        pendingChannelUid), both of which live on this processor. A second
        selected-strip field would be a second authority on the same fact. */
    /** Meter left this enum in the 8b layout pass: the meter became permanent
        strip chrome (a mixer strip always shows its meter), so the toggle
        now switches only the upper data area. The VALUES ARE LOAD-BEARING:
        Chain stays 2 because projects saved before 8b persisted 0/1/2 for
        numbers/meter/chain, and renumbering would silently turn a saved
        CHAIN into something else. Value 1 (meter) no longer exists; the
        mapper below owns what saved integers become. */
    enum class LinkMixerContent { Numbers = 0, Chain = 2 };
    /** THE migration for persisted linkMixerContent, one authority, pure and
        testable: 0 -> Numbers, 1 (the removed meter mode) -> Numbers
        (the meter is now always visible, so Numbers is the view closest to
        what that project saved), 2 -> Chain, anything else -> Numbers. */
    static LinkMixerContent linkMixerContentFromSaved(int saved)
    {
        return saved == 2 ? LinkMixerContent::Chain : LinkMixerContent::Numbers;
    }
    LinkMixerContent linkMixerContent = LinkMixerContent::Numbers;

    // Mixer fader mode (18 Aug 2026): Post = each fader controls its channel's
    // post-chain output level (as always); Pre = each fader controls that
    // channel's PRE-chain gain (headroom into the rack). Per Link, persisted.
    // The meters stay post-chain in both: you trim the input while watching
    // the output. Repoints the ONE strip, never a second row.
    enum class LinkFaderMode { Post = 0, Pre = 1 };
    LinkFaderMode linkFaderMode = LinkFaderMode::Post;
    bool linkMixerWide = false;        // false = narrow strips (the reference)

    /** LINK MIXER rack cache (step 9). ON THE PROCESSOR, not the editor,
        because Logic recreates the editor on every Link window switch and a
        cache that died with the editor would re-read every sidecar on every
        switch. WRITTEN only by the editor's refreshLinkRackCache (message
        thread, ~1Hz plus a forced pass on entering CHAIN mode); READ only by
        paint, which never touches a file. The parser stays
        EchoJayEditor::readLinkRackSidecar, the one sidecar reader. `valid`
        distinguishes "file existed and parsed" from "missing/unreadable",
        which is the no-data vs empty-rack honesty line. */
    struct LinkRackCacheEntry {
        LinkShm::RackSidecar rack;
        bool     valid  = false;
        uint32_t readMs = 0;
    };
    std::map<juce::String, LinkRackCacheEntry> linkRackCache;

    /** LINK MIXER per-strip rack SCROLL offset in pixels, keyed the same way
        the cache is (Link uid, or "MIX BUS" for the pinned strip). ON THE
        PROCESSOR for the standing reason: Logic recreates the editor on
        every Link window switch, and a scroll position that reset on every
        switch would be worse than no scrolling. Written only by the wheel
        handler, read only by the row layout, and CLAMPED there, so a stale
        offset left by a rack that shrank can never misplace a block. */
    std::map<juce::String, int> linkChainScroll;

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
    // §5a-R orchestrator internals: the ack poll outlives editors, so its
    // lambdas hold a weak token instead of `this` — a plugin unloaded
    // mid-poll must drop the chain, never call into freed memory.
    std::shared_ptr<bool> borrowAliveToken_ { std::make_shared<bool>(true) };
    AlignDelay alignPre_, alignPost_;
    juce::SmoothedValue<float> borrowCtxMix_ { 0.0f };
    bool borrowApplyReleaseOnFail_ = false;   // switchable mid-flight (close)
    void borrowApplyFinish(bool applied, const juce::String& why,
                           bool restored);

    MeterEngine meterEngine;       // Live meters (always running — live input only)
    MeterEngine captureEngine;     // Capture pass meters (reset each capture)
    MeterEngine abMeterEngine;     // AB playback spectrum only (used by Compare playing-slot panel)
    MeterEngine cmpMeter[2];       // Compare stream meters (one per slot)
    PluginScanner pluginScanner;
    ReferenceAnalyser refAnalyser;
    WaveformRecorder waveformRecorder; // Audio recording + waveform thumbnail
    ChainHost chainHost;           // Plugin chain hosting (CHAIN tab)
    // Declared AFTER chainHost and before nothing that uses it at
    // construction. See getApi() above for the lifetime argument.
    EchoJayAPI api;

    /** The process-wide poller. juce::SharedResourcePointer creates it on
        first use and destroys it when the last processor releases it, so the
        lifetime is refcounted rather than a leaked static. */
    juce::SharedResourcePointer<DashPollShared> dashPoll;

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
    juce::String nextCaptureName_;      // item 1: press-time override (single source)
    juce::String nextCaptureScopeUid_;  // item 1: press-time channel scope

    // Auto-feedback
    mutable std::atomic<bool> autoFeedbackReady { false };

    // Silence detection (triggers auto-stop when DAW stops)
    std::atomic<bool> audioSilent { true };
    std::atomic<bool> transportPlaying { false };
    bool wasTransportPlaying = false;
    int silenceCounter = 0;
    bool wasReceivingAudio = false;

    // ---- Self key detection internals (§6.1) -----------------------------
    // The Link's §5.1 scheduler, verbatim semantics: one committed 8 s pass
    // every ~30 s, armed only while the transport rolls and the channel is
    // above the signal floor; §5.4 invalidation (section jump / long gap /
    // RE-ANALYSE) makes the next pass due immediately. Runs ONLY while the
    // declared role is a music bus — the tap is not even fed otherwise.
    // Engine before worker (the worker references the engine; destroyed
    // first).
    echojay::KeyEngine selfKeyEngine_;
    EedKeyWorker       selfKeyWorker_ { selfKeyEngine_ };
    static constexpr float    kSelfKeyWindowS     = 8.0f;
    static constexpr uint32_t kSelfKeyIntervalMs  = 30000;
    static constexpr float    kSelfKeyFloorLufs   = -55.0f;
    static constexpr double   kSelfKeyJumpSeconds = 30.0;
    static constexpr uint32_t kSelfKeyLongGapMs   = 120000;
    std::atomic<double> transportTimeS_ { 0.0 };   // audio thread -> scheduler
    std::atomic<bool>   selfKeyJump_    { false };
    // §7: pin == "self" forces the self tap/scheduler past the role gate.
    // Atomic mirror because the audio thread reads it every block.
    std::atomic<bool>   selfKeyForced_  { false };
    juce::String keySourcePin_;                    // message thread; persisted
    juce::String keySourcePinLabel_;
    uint32_t lastSelfKeyArmMs_     = 0;            // message thread only
    uint32_t selfKeyLastPlayingMs_ = 0;
    bool     selfKeyWasPlaying_    = false;
    int      selfKeyStallTicks_    = 0;
    void timerCallback() override;                 // 1 Hz: the scheduler
    void scheduleSelfKeyPass();
    // Track name from updateTrackProperties (see there); the timer applies it
    juce::CriticalSection hostTrackNameLock_;
    juce::String          hostTrackNamePending_;
    std::atomic<bool>     hostTrackNameDirty_ { false };
    void applyHostTrackNameIfDirty();              // message thread

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
        bool         dialCapable = false; // Link applies settings_structured.
                                          // FALSE from every existing Link and
                                          // that is the correct default: an old
                                          // Link cannot announce anything, so
                                          // absence must mean "do not send
                                          // controls", never "check a version".
        bool heartbeatFresh = true;       // heartbeat advanced within ~3s.
                                          // FALSE = the process stopped
                                          // answering: the strip renders the
                                          // distinct "gone" state until the
                                          // ~30s reap removes the row. This
                                          // separates DEAD from SILENT; it
                                          // cannot flag deleted-but-undo-held,
                                          // because that instance is genuinely
                                          // alive (Logic keeps deleted
                                          // channels' plugins running for
                                          // undo) — no signal of ours can see
                                          // the arrange page.
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
    juce::int64  lastFileReapMs_ = 0;   // dead-uid file sweep throttle (~5 min)
    juce::String ctxCapSetKey_;          // §8.3: listed-uid set fingerprint
    std::map<juce::String, bool> ctxCapCache_;   // uid -> inContextCapable

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
    struct SlotProbeState { uint32_t lastHb = 0; int staleCycles = 0;
                            LinkShm::RegLiveness live; };
    std::array<SlotProbeState, kMaxLinkSlots> slotProbeStates;

    // UI snapshot (message thread only)
    std::vector<LinkSlotInfo> linkSlotInfos;

    // Bus trim internals (see the public block): value, smoother, snap flag
    std::atomic<float> busGainDb_ { 0.0f };
    juce::LinearSmoothedValue<float> busGainSmoothed_ { 1.0f };
    std::atomic<bool> busGainSnapPending_ { false };
    bool applyBusGainSmoothed(juce::AudioBuffer<float>& buffer);

    // Per-link capture channels — message thread writes, audio thread reads via spinlock
    std::vector<std::unique_ptr<LinkCaptureChannel>> linkCaptureChannels;
    juce::SpinLock                                   linkCaptureSpinLock;
    double hostSampleRate_    = 44100.0;
    int    hostSamplesPerBlock_ = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoJayProcessor)
};
