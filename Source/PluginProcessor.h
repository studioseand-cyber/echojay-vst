#pragma once
#include <JuceHeader.h>
#include <map>
#include <thread>
#include <atomic>
#include "MeterEngine.h"
#include "EedLatencyLog.h"
#include "PluginScanner.h"
#include "ReferenceAnalyser.h"
#include "EJReferenceRows.h"   // reference folders and the selected scope
#include "WaveformRecorder.h"
#include "ChainHost.h"
#include "EJCalibLoop.h"   // 21t-d: the calibration loop (shared with the Link)
#include "EJCmdRing.h"   // stage 3: the value ring, producer side
#include "EJStateRoot.h"   // echojay::userAppData() - every state path goes through the root so a runner can isolate it
#include "EJLevelRecord.h"  // 21t-i: the stored level record (shared with the Link)
#include "EchoJayAPI.h"
#include "EchoJayWorkspace.h"   // 22 Oct 2026: the chat store is the PROCESSOR'S, so a windowless run can append
#include "DashPoll.h"
#include "LinkShm.h"
#include "EJShadowView.h"   // stage B of the muted shadow: the V2's own copy of the slot in view
#include "EchoJayUndoHistory.h"   // 21n item 3
#include "EedKeyEngine.h"   // self-detection on music-bus roles (§6.1)
#include "EJChainRole.h"    // 21t-m item 5: the chain's role, from its three sources
#include "EedKeyWorker.h"
#include "LoudnessLoop.h"
#include "EJReferenceReconcile.h"  // ReferenceIndex, held as refLibrary_
#include "EJCaptureGuard.h"  // output substitution: the shipped predicate + record field
#include "EJSpectralEvidence.h" // spectral provenance: reduction, window, the band reduction
#include "EJPlaybackSim.h"    // the inline monitoring stage, below the meter tap

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
    // ===== THE ACCUMULATED MACRO BANDS (Phase 1b commit 3) =====
    // The whole-capture mean of the six pink-referenced bands, in POWER, read
    // off captureEngine at stopCapture. It sits BESIDE averagedData rather than
    // inside it, because averagedData.macroBandDb is the ballistic tail at the
    // moment of stopping and this commit changes no consumer of that field.
    //
    // WHY captureEngine AND NOT meterEngine: captureEngine is reset at capture
    // start, so its accumulator covers the capture and nothing before it. The
    // spectrum rebuild beside this one sums meterEngine's ballistic spectrum,
    // which carries up to ~450 ms of pre-capture history into its first frames.
    // Same blocks, cleaner span.
    std::array<float, 6> macroBandAccum { -120, -120, -120, -120, -120, -120 };
    bool                 hasMacroBandAccum = false;   // false = not measured
    float                macroAccumSeconds = 0.0f;
    int                  macroAccumBlocks  = 0;
    echojay::SpectralReduction macroAccumReduction = echojay::SpectralReduction::Unknown;
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
    //
    // DEFAULTED TO THE UNSET SENTINEL, NOT TO {} (11 Sep 2026). Zero-filled,
    // these read as 0 dB in every bin: not a quiet spectrum but an impossibly
    // loud flat one. Read without the gate they produce a confident wrong
    // answer in two shapes. With ONE side zero-filled, that side's own maximum
    // is 0 so its contribution cancels and the delta becomes the OTHER side's
    // tilt, reported as a large deficit in every band but its loudest. With
    // both zero-filled it reads as perfect agreement. Neither is
    // distinguishable from a measurement. hasDualSpectrum is still the gate and
    // all three readers honour it; the sentinel catches the day one does not.
    std::array<float, 64> peakSpectrum = echojay::unsetSpectrum();
    std::array<float, 64> avgSpectrum  = echojay::unsetSpectrum();

    // FALSE ON EVERY RESTORED SNAPSHOT, not merely on old save files. Neither
    // spectrum is persisted (getStateInformation writes eqCurve only) and the
    // restore path never sets this flag, so a capture reloaded with the session
    // has lost both reductions permanently and can only be compared with the
    // reduction caveat. Persisting the pair is a state-blob change and is filed
    // on COMPARE_REFERENCE_PLAN's open items rather than done here.
    bool hasDualSpectrum = false;

    // WHAT WAS REPLACING THE OUTPUT WHEN THIS WAS CAPTURED. "" = nothing, and
    // that is every capture today: startCapture REFUSES while A/B playback, a
    // compare stream or codec preview is running, because such a capture
    // measures a file and reports it as the mix.
    //
    // WRITTEN FOR A FUTURE THE GUARD CURRENTLY FORBIDS. Section 4 of the
    // compare plan wants captures taken THROUGH a playback simulation on
    // purpose; when that relaxes the guard, this field is what keeps the
    // result honest, and every figure derived from the capture can carry it.
    // Adding it then instead of now is how the same defect arrives twice.
    // Tokens are echojay::outputSubstitutionKey (see EJCaptureGuard.h).
    juce::String outputSubstitution;

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

/** ITEM 0b (15 Oct 2026): the agent's session, executor and record. Declared here and INCLUDED
    ONLY IN THE .cpp, so moving the agent onto the processor does not drag EJAgentClient.h into
    every translation unit that needs a processor. */
class EJAgentHome;

class EchoJayProcessor : public juce::AudioProcessor,
                         private juce::Timer   // §6.1 self-key scheduler (1 Hz)
{
public:
    EchoJayProcessor();
    ~EchoJayProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override { EJ_LAT_LOG ("top: reset() from the host"); chainHost.requestReset(); }   // round 48: AudioUnitReset reaches every hosted slot (DEFECT_PRESS_PLAY_PHASING)
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
    // 21t-m item 5 (29 Sep 2026 ruling, the music gate): SUPERSEDED as the answer, kept as ONE INPUT to it.
    // "The chain is THE MUSIC when chainRole() says bus AND none of these says it is a vocal or rhythm bus: the
    // prompt answered VocalBus or DrumBus, or the track name contains vocal/vox/bv/harmony/drum/perc." So a
    // start-prompt answer of Mix Bus no longer ANSWERS the music question on its own - it only makes the chain a
    // bus, which every other source can do too. The decision is echojay::decideChainIsMusic; this predicate is
    // gone as a reader and nothing else calls it.
    /** THE ONE READER of the music question, and it reads ONE ATOMIC.
        21t-m (29 Sep 2026): this is called from processBlock (PluginProcessor.cpp:1399, the audio thread) on
        every block. The first cut of the item-5 ruling made it compute the answer here - which meant calling
        ChainHost::getHostTrackName(), a juce::String RETURNED BY VALUE, on the audio thread: a ref-counted copy
        (a heap operation in the real-time path) and an unsynchronised read of a string the message thread
        mutates (ChainHost.cpp:2994). A racing ref count corrupts the shared object, and four Link rigs went red
        with a juce::String reading as neither its value nor empty.
        The ruling said the role is the one reader; it did not say which thread, and the implementation followed
        it onto the wrong one. The verdict is computed by publishChainRole() - which already runs at every point
        the three inputs change - and stored here. Nothing in this function may become more than a load: there is
        a guard in the gate that fails if it does. */
    bool selfKeyRoleIsMusic() const noexcept { return selfKeyIsMusic_.load (std::memory_order_relaxed); }

    // ---- 21t-m item 5 (29 Sep 2026 ruling): THE ROLE HAS THREE SOURCES ------------------------------------
    // "isBusRole reads only the start-prompt channelType, so a track named 'Mix Bus' with the prompt unanswered
    // is a channel - which is today's demo." The decision is echojay::decideChainRole (EJChainRole.h) and this
    // is where its three inputs are gathered. NOTHING reads channelType for a ROLE question any more; the one
    // place that still reads it is the MUSIC question, and that is now decided by echojay::decideChainIsMusic
    // too: a vocal bus is a bus and is not the music, so the music gate is chainRole() NARROWED, not channelType.
    echojay::ChainRole chainRole() const;
    /** The Link's placement selector, as this instance knows it: 1 bus, 2 channel/insert, 0 unset. V2 has no
        selector of its own and leaves it 0, which contributes nothing to the decision. */
    void setSelfPlacement (int p) { selfPlacement_ = p; }
    int  selfPlacement() const noexcept { return selfPlacement_; }

    // ---- 21t-m item 2 (29 Sep 2026 ruling): A BUS IS NOT A CHANNEL ------------------------------------------
    // "The whole-chain hold and the stored-record restore are for channel roles only - on Mix Bus, master or any
    // bus role nothing is written to the chain output and no restore lands on it; the chain's last stage sets the
    // level." The declaration already exists and Sean's session carries it: the 12:59 build logged "declared Mix
    // Bus - EchoJay is ON the music bus", and the same instance then restored a saved chain-output tally onto it
    // ("EJLevels: pending restore, chain in=y out=y slots=0 track=\"Mix Bus\""). This is the predicate that stops
    // that. Broader than the old isMusicBusRole: a vocal bus and a drum bus are buses too, and the rule says ANY
    // bus role - the music question is the narrowed one (decideChainIsMusic), not this.
    static bool isBusRole(ChannelType t)
    {
        return t == ChannelType::FullMix      || t == ChannelType::MasterBus
            || t == ChannelType::MusicBus     || t == ChannelType::InstrumentBus
            || t == ChannelType::VocalBus     || t == ChannelType::DrumBus
            || t == ChannelType::GuitarBus    || t == ChannelType::SynthBus;
    }
    /** 21t-m item 5: kept as the PROMPT source only - the whole-chain answer is chainRole(). */
    bool promptSaysBus() const { return isBusRole(channelType); }
    /** Push the declared role down to the ChainHost (see the .cpp). */
    void publishChainRole();

    // Which spectrum statistic this channel type takes (METER SNAPSHOT v3).
    // Sustained full-range material gives a representative tonal balance when
    // AVERAGED; transient sources are diluted by averaging and need PEAK-HOLD
    // to show what is actually there, which is why Drum Bus sits on the peak
    // side despite being a bus. Extracted from the capture path so the chat
    // injection and the capture payload cannot drift onto different rules;
    // stopCapture reads it too.
    static bool spectrumUsesAverage(ChannelType t)
    {
        return t == ChannelType::FullMix || t == ChannelType::MasterBus
            || t == ChannelType::MusicBus || t == ChannelType::InstrumentBus;
    }
    bool spectrumUsesAverage() const { return spectrumUsesAverage(channelType); }
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
    // COMMIT 4 (17 Sep 2026): keyShowRelative - the process-wide display
    // pref (echojay::KeyDisplayPrefs), persisted in this instance's state
    // beside keySourcePin (the nearest key setting's pattern).
    void setKeyShowRelative(bool on);

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
    // ===== 21n item 3 (22 Sep 2026): ONE plugin-wide undo history (design accepted as ten lines) =====
    echojay::UndoHistory& undoHistory() { return undoHistory_; }
    bool applyUndoEntry(echojay::UndoEntry& e, bool toBefore);          // the dispatcher (public for the guard)
    // the editor records the Link commands it sends; the dispatcher re-sends them as a ctrl-cmd
    /** FIX 4(a), 14 Oct: a HAND move on the pre-gain, as one undo step per drag. */
    /** `target` names the rack: "" is this instance's own, a uid is a borrowed one. Defaulted so the
        three editor call sites (the knob, both windows) are unchanged. */
    void recordPreGainUndo(float beforeDb, bool beforeUserSet, float afterDb, bool afterUserSet,
                           const juce::String& target = {});
    void recordLinkActiveUndo(const juce::String& uid, bool before, bool after);
    void recordLinkGainUndo(const juce::String& uid, float before, float after);
    /** 21t-l item 1: ONE undo entry for a whole op. A headroom op moves every Link the scope selects; the
        per-Link entries a fader drag records would make undo walk them one at a time, so they are suppressed
        for the duration and one "headroom" entry is pushed instead. Same shape as the group fader's groupGain:
        the entry carries the OFFSET and undo re-applies its inverse to the same scope. */
    struct ScopedLinkGainUndoSuppress
    {
        explicit ScopedLinkGainUndoSuppress (EchoJayProcessor& p) : p_ (p) { p_.linkGainUndoSuppressed_ = true; }
        ~ScopedLinkGainUndoSuppress() { p_.linkGainUndoSuppressed_ = false; }
        EchoJayProcessor& p_;
    };
    void recordHeadroomUndo(const juce::String& scopeRole, float offsetDb, const juce::String& label);
    void writeLinkCtrlCommand(const juce::String& uid, const juce::String& field, const juce::var& value);
    juce::String lastUndoStatus() const { return lastUndoStatus_; }
    void wireUndoHooks(ChainHost& h, const juce::String& rackUid);       // chain / scalar / dial hooks on a ChainHost

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

    /** ---- THE CHAT STORE, ON THE PROCESSOR (Sean's fault of 22 Oct 09:24) -------------------
        *"a run with no window open does NOT come back in the chat ... the build WAS on the rack,
         but the chat showed only my message: no agent card, no steps, no closing, no chips, no
         Undo all."*

        THE CAUSE, and it is structural rather than a missed call: this store was a member of the
        EDITOR. A run's card was appended to it by the window, at the END of the run - so a run that
        finished with no window had nowhere to write, its card lived only in `chatHistory` (the
        window-crossing replay list), and the first chat switch dropped that list on the floor:
            EJStores: dropped 1 unsynced row(s) that belong to chat 1792657430720
        His instruction is to fix it at the cause "through the single stamp (the store appends first,
        with no window involved)", and a stamp cannot append to a store that does not exist.

        So the store lives here, and the editor holds a REFERENCE to it - the same move EchoJayAPI
        made above, with the same lifetime guarantee (JUCE destroys the editor before the processor,
        and this is a plain value member so it outlives every window). Its constructor does no work
        and starts no timer, so owning it costs nothing until something loads or appends.

        Every one of the editor's 200-odd `workspace.` call sites is unchanged. */
    EchoJayWorkspace workspace { api };

    // ---- ITEM 0b (Sean, 14 Oct 2026): THE RUN SURVIVES THE WINDOW -------------------------------
    //
    // The agent's client, executor and the run's record live HERE, not on the editor, because an
    // editor is allowed to disappear in the middle of a run and until now the run disappeared with
    // it: the socket, the steps and the undo tokens all went, with no error, because nothing was
    // left to report one. See EJAgentHome.h.
    //
    // BUILT ON FIRST USE, not in the constructor: the sinks read chainHost, api and the undo
    // history, and the first caller is the editor's constructor - which runs after this object is
    // fully built. That is the same ordering the editor had, kept rather than re-reasoned.
    EJAgentHome& agentHome();
    /** Without building one. The rig and the state publisher ask "is there a run?" on a timer, and
        a question must not be able to construct the thing it is asking about. */
    EJAgentHome* agentHomeIfBuilt() noexcept { return agentHome_.get(); }
    // 21r item 3: the launch rescan question is asked at most once per PROCESS, not once per editor - closing and
    // reopening the window is not a new launch. Public because the editor asks the question and the processor
    // outlives it.
    bool rescanOffered_ = false;

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
                                     float durA, float durB, bool numbersOnly,
                                     const echojay::SpectralEvidence& sa,
                                     const echojay::SpectralEvidence& sb) const;
    // Figure-CARD data (client-rendered at compose time): both sources' figures
    // + labels + a cross-scope flag, enough to redraw the card identically on
    // reload. Only-present keys; absent = N/A. Shares computeCompareFig with the
    // text table so the card can never disagree with the model's numbers.
juce::String buildCompareFiguresJson(const MeterData& a, const MeterData& b,
                                         const juce::String& la, const juce::String& lb,
                                         bool crossScope,
                                         const echojay::SpectralEvidence& sa,
                                         const echojay::SpectralEvidence& sb) const;

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
    // 21r item 7: has this instance ever answered "which channel is this on?". Persisted; the question is asked
    // only when this is false, and the header dropdown can still change the answer afterwards.
    bool isChannelChosen() const { return channelChosen; }
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
    // Which feature, if any, is replacing the output buffer right now.
    // startCapture refuses on it; the editor uses it to state the reason.
    echojay::OutputSubstitution activeOutputSubstitution() const;

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
    // ---- 11 Oct 2026, THE REMOVALS: THE ALIGNMENT BUDGET AND ITS DELAY LINES --------------------------
    // kBorrowAlignBudgetFrames (16384 samples, 341.3 ms at 48 k), alignPad() and the two AlignDelay ring
    // delays are gone. They realised one idea: hold the passthrough at a CONSTANT delay so an injected copy
    // of a Link's rack could be summed in perfectly aligned, whatever that rack's own latency. The figure was
    // measured rather than guessed (this machine's catalogue, defaults, 48 k: worst single slot found was
    // Ozone 12 Low End Focus at 12,799), and it was reported to the host as added latency whenever a capable
    // Link was present. Nothing is injected now, so nothing needs aligning and nothing is reported.
    //
    // kEditCushionFrames above STAYS: it is the ring read target for the slot-edit audition and the solo
    // press, which are their own features and still consume rings.
    static constexpr uint32_t kEditReseekTrip    = 8192;

    // ---- THE SHADOW VIEW (stage B wired up, 23 Oct 2026) -----------------------------------
    //
    // *"when I select a Link's plugin in the V2, I see that plugin's real editor live in the V2
    //  (moving meters and gain reduction) without opening the Link's window ... No control through
    //  it yet: clicks do nothing, and it must never take the rack, stop a run or change
    //  ownership."*
    //
    // The audition's machinery WITHOUT the lease, which is what the plan asked for: a prepared copy,
    // fed from the Link's shadow ring, output discarded. No setRackLockWant, no borrow, no lease
    // file, nothing a release has to undo - his 08:44 fault was an ownership change nobody asked
    // for, and this feature may never be another one.
    //
    // docs/SHADOW_VIEW_STAGES_2026-10-23.md says what this covers and what control still needs.
    echojay::ShadowView shadowView_;
    void*        shadowRingMap_ = nullptr;
    int          shadowRingFd_  = -1;
    juce::String shadowRingPath_;
    juce::String shadowSeedNote_;      // FOLLOW layer 1: why this view is on defaults, if it is
    // ---- FOLLOW layer 2: the Link's parameter values, read at the editor's rate --------------
    void*        shadowValMap_ = nullptr;
    int          shadowValFd_  = -1;
    juce::String shadowValPath_;
    std::vector<float> shadowValRead_;     // one frame's values, reused, never allocated on a tick
    juce::uint32 shadowValSeqSeen_ = 0;    // the last STABLE seq applied; unchanged = no new frame
    juce::uint32 shadowCopyDigest_ = 0;    // the copy's schema, taken once when the view starts
    juce::String shadowFollowNote_;        // what he reads when the knobs are NOT being followed
    bool         shadowFollowSaid_ = false;
    int          shadowFollowFrames_ = 0, shadowFollowWrites_ = 0;
    /** Install a prepared copy and start consuming that Link's shadow ring. The caller has already
        asked the Link to tap the slot (the control channel's `shadowSlot`) and seen its ack. FALSE
        with `whyNot` set changes nothing - the view falls back to the static picture, which is the
        designed failure and not an error. */
    /** `seedState` is the REAL plugin's settings, pulled from the Link in the same request that
        asked for the tap (FOLLOW layer 1, 24 Oct 2026). Empty means "open on the defaults", and
        shadowSeedNote() then carries the one line he reads about why. */
    bool shadowBegin (const juce::String& uid, int slot0, const juce::String& pluginName,
                      std::unique_ptr<juce::AudioProcessor> inst,
                      const juce::MemoryBlock& seedState, juce::String& whyNot);
    /** What happened to the settings this view opened on: empty when they were applied, otherwise
        the plain line the card shows. ONE PLACE, so the ack's refusal and the plugin's refusal
        cannot be worded differently by two call sites. */
    juce::String shadowSeedNote() const { return shadowSeedNote_; }
    /** FOLLOW layer 2: read the Link's latest values for the tapped slot and write the CHANGED
        ones into the copy. Message thread, called from the editor's own timer - a parameter write
        is a plugin call and belongs nowhere else. Cheap and silent when there is no new frame. */
    void shadowFollowTick();
    /** Empty while the knobs are following; otherwise the one plain line saying they are not. */
    juce::String shadowFollowNote() const { return shadowFollowNote_; }
    int shadowFollowWrites() const noexcept { return shadowFollowWrites_; }
    /** Stop consuming and release the copy. The ring is unmapped AFTER the view has let go. */
    void shadowEnd();
    bool shadowRunning() const noexcept { return shadowView_.running(); }
    juce::String shadowUid() const { return shadowView_.uid(); }
    int  shadowSlot() const noexcept { return shadowView_.slot(); }
    juce::String shadowPluginName() const { return shadowView_.pluginName(); }
    juce::int64 shadowFramesPumped() const noexcept { return shadowView_.framesPumped(); }
    /** The copy's own editor, for the V2's panel. Message thread; nullptr when nothing is running. */
    juce::AudioProcessorEditor* shadowCreateEditor();

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
    // RULING 1 (21s-b, 24 Sep 2026): republish THIS rack's sidecar from the borrowed host, so the Link's published
    // description of its own rack matches what the lease holder just built into it. Called after any build or edit
    // on a borrowed rack, BEFORE the lock is released - the edit preflight and the chat's [CURRENT CHAIN] block
    // both read that file, and until now they read an empty one while the rack was held.
    bool republishBorrowedRackSidecar();
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
    // 20 Sep 2026 - THE DELETED-CHAIN BUG. Structural edits on a BORROWED rack (add / remove / move / replace / bypass) used to
    // live only in this plugin's BorrowHost until deselect/close computed a plan; a host save in between saved the Link's
    // pre-edit chain, and it came back on reopen. Now every structural edit is PUSHED to the Link at once over the chain-command
    // transport (the same writer the Link tab's per-slot edits use, chain-cmd-<uid>.json v:2), carrying this session's leaseId so
    // the Link accepts it under its own lease. After the Link acks, the session is REBASED (base identity = the live rack,
    // origins = identity) so the deselect plan carries no structural op - only state commits.
    //   op: "add" | "remove" | "move" | "replace" | "bypass"; slot0/to0 are 0-based indices in the rack BEFORE the edit
    //   (add: -1 = append); name for add/replace; on for bypass; baseSlotsBefore = the slot names before the edit.
    // Returns the command seq (>0) or -1 (no session / no shared dir). Logs "EJLink: push <op> slot=<n> count=<a>-><b> id=<uid>".
    int  borrowPushStructuralEdit(const juce::String& op, int slot0, int to0, bool on, const juce::String& name,
                                  const juce::StringArray& baseSlotsBefore, int countBefore);
    // ---- 11 Oct 2026: A BUILD IS A COMMAND TOO ---------------------------------------------------------
    // The one edit on a Link rack that was NOT a command. Every other - add, remove, move, bypass, replace -
    // went through the push above the moment the user made it, acked and rebased (20 Sep, rebase-on-ack). A
    // BUILD did not: it landed in the mirror and waited for the rack to be left, "the Link refuses chain-cmds
    // while leased" (sendChainToLink's own note). That refusal only ever applied to a sender that was NOT the
    // holder, and the holder is who is building - so the build had been free to travel for three weeks.
    //
    // It is Sean's 4 October bug at the root: a chain built on a Link and SAVED without switching racks lived
    // only on this side. The save fix carried it across (borrowedRacks, and the revision tie-break that keeps
    // a deleted rack deleted); this removes the reason it had to be carried. Nothing lives only in the mirror,
    // so that whole class of loss cannot recur - and the migration below stays for the projects that still
    // hold one.
    //
    // ONE QUEUE for every structural edit: the same ack poll, the same rebase, the same "unacked in 5 s, the
    // user is told rather than the edit quietly waiting" path. Returns the seq (>0) or -1.
    int  borrowPushBuiltChain(const juce::var& editOps, const juce::StringArray& baseSlotsBefore,
                              int countBefore, int countAfter);
    // The ONE chain-command writer (chain-cmd-<uid>.json v:2): editOps + baseSlots (+ leaseId when the sender holds the rack's
    // lease). Used by the editor's Link-tab edit path and by the borrowed-rack push above. Returns the seq or -1.
    /** `intent` is the command's purpose when it has one beyond "edit these ops": "build" makes the
        Link arm its post-build level match, exactly as the borrow push's own intent does (item 4,
        14 Oct). Additive and ignorable - an older Link reads no intent and behaves as it does
        today. Empty for an ordinary edit, which must never re-level a rack. */
    int  writeChainEditCommand(const juce::String& linkUid, const juce::var& editOps, const juce::var& baseSlots,
                               const juce::String& sourceNote, const juce::String& leaseId,
                               const juce::String& intent = {});
    // ---- ITEM 1 (Sean, 13 Oct 2026): "SENT" IS NOT "APPLIED" --------------------------------------------
    // The agent's apply sink reported success on `seq >= 0`, which is the return of a FILE WRITE. Sean
    // approved a build for the Kick, was told it landed, and the Kick's Link had nothing on it - then the
    // agent read the empty rack for 18 rounds. A command written is a command written; whether the Link took
    // it is in chain-ack-<uid>.json, which this build already wrote and only the borrow path ever read.
    //
    // Bounded, and the bound is reported in the sentence: a caller that gets `false` is told whether the Link
    // refused it (with the Link's own reason) or never answered, because those need different next moves.
    // slotsAfter is -1 when the Link is an older build that does not state it.
    /** ---- SEAN'S CHECK 5 (15 Oct 2026): THE ACK CARRIES A TECHNICAL ACCOUNT TOO ----------------
        It used to hand back ONE sentence, built from the ack's `status` and `reason` alone - and a
        BUILD ack has no `reason`, so "the rack refused it (partial)" was the whole of what anyone
        learned. The Link had written the real line into `perPluginResults` ("kHs Transient Shaper"
        didn't load (An OS error occurred during initialisation of the plug-in (-1))...) and nothing
        read it. `technical` is that, verbatim, for the model and for the line's Details disclosure;
        `sentence` is the one plain line, now with the rack's own reason in it. */
    void awaitChainAck(const juce::String& linkUid, int seq,
                       std::function<void(bool ok, juce::String sentence, juce::String technical,
                                          int slotsAfter)> cb,
                       int maxWaitMs = 8000);
    // ---- THE VALUE RING, PRODUCER SIDE (remote control stage 3, 10 Oct 2026) ----------------------------
    // The FILE above is for decisions; this is for values. The rule that picks the channel is one question -
    // does the next one supersede this one? A drag has no meaningful ack because the next frame supersedes it,
    // so it goes here and gets none.
    //
    // THE GESTURE IS THE UNIT. ringBegin / ringValue / ringEnd frame one drag, and the Link's coalescer keeps
    // the newest STREAM frame per control while never discarding a boundary - that is what makes a 400-frame
    // drag cost one write per control and still produce exactly one undo step.
    //
    // Every frame carries the rack's structureRevision in its flags, so a V2 holding a stale sidecar cannot
    // apply a value to the wrong parameter: the Link discards the mismatch with a count. Same class of
    // mistake baseSlots guards on the chain channel, same treatment.
    /** Opens a gesture on a rack and returns its id, or 0 if the rack has no ring (the caller then uses the
        file channel, which is the correct fallback and not a failure). */
    uint32_t ringBeginGesture (const juce::String& linkUid, int kind, int slot, int paramId,
                               double value, uint32_t structureRevision);
    /** A value inside an open gesture. Cheap by design: call it as often as the mouse moves. */
    bool     ringStreamValue (const juce::String& linkUid, uint32_t gesture, int kind, int slot, int paramId,
                              double value, uint32_t structureRevision);
    /** The settled value, and the authority for the gesture. */
    bool     ringEndGesture  (const juce::String& linkUid, uint32_t gesture, int kind, int slot, int paramId,
                              double value, uint32_t structureRevision);
    /** STAGE 5: the settled value AND one undo step for the whole gesture. This is what a release should call:
        the plain ringEndGesture above records nothing, which is right for a restore (an undo must not push an
        undo) and wrong for a user's own drag. Carries {uid, what, slot, before, after} as the plan specifies,
        as a CHAIN entry in the plugin-wide history so one Undo button walks local and remote edits in one
        order. */
    bool     ringEndGestureWithUndo (const juce::String& linkUid, uint32_t gesture, int kind, int slot,
                                     int paramId, double beforeValue, double afterValue,
                                     uint32_t structureRevision, const juce::String& label = {});
    // After a pushed edit is acked: the session's base becomes the live rack (created slots gain a record so their state edits
    // still commit at deselect). Public for the guard.
    void borrowRebaseAfterPush();
    /** ---- THE COPY IS STALE AND ONLY A FRESH READ CAN FIX IT (Sean's 09:59, 21 Oct 2026) ----
        Raised when an editor open is refused because the hosted copy's slot identity does not match
        the Link's published one. The EDITOR owns the repair (startBorrow re-reads the Link's rack,
        which is what his close-and-reopen was doing by hand), so the processor states the fact and
        the editor acts on it.

        NOT borrowRebaseAfterPush: that re-stamps the mirror's own contents as the baseline, which on
        a drifted copy blesses the drift. */
    std::function<void (const juce::String& uid)> onBorrowCopyStale;
    // 20 Sep 2026 (rebase-on-ack, the owed fix): a pushed structural edit is PENDING until the Link acks it "ok"; the session
    // rebases in the ack callback, never on write success. The chain-cmd transport has ONE file per Link, so pushes are QUEUED
    // and written one at a time (the next is written when the previous is acked); a missing or "failed" ack keeps the op at the
    // head of the queue (polled while the session is live). Deselect/close re-sends every pending op, in order, waiting for each
    // ack, BEFORE its state commits. Each command carries an id (<uid>-<seq>); the Link acks a repeated id "ok" without
    // re-applying it, so a lost ack never applies a delete twice.
#define EJ_V2_ACK_QUEUE 1   // 20 Sep 2026: the harness's RED-by-name key
    struct BorrowPendingPush { int seq = 0; juce::String id, op; juce::var editOps, baseSlots; int attempts = 0; };
    std::vector<BorrowPendingPush> borrowPendingPushes_;
    // R2 (21t-b, 25 Sep 2026): edits the Link did not acknowledge in time are PARKED by rack, not thrown away.
    // The rack is released so the user can move on (a handover never locks anyone out) and the queue is re-armed
    // and retried the next time that rack is engaged - which is what "retried when the Link answers" means when
    // the Link only answers while it is leased.
    std::map<juce::String, std::vector<BorrowPendingPush>> borrowParkedPushes_;
    juce::String borrowParkedNotice_;   // see takeBorrowParkedNotice
    // 21t-d: the loop for THIS instance's own rack (the mix bus case); a Link rack's loop lives on its sidecar.
    echojay::CalibLoop ownCalib_;
    // (i) 30 Sep 2026, B's contract: EVERY COMPRESSOR IN A BUILD GETS ITS OWN HOLD. `calibrations` carries one
    // block per compressor; calibrations[0] is the PRIMARY and lives in ownCalib_ exactly as the single block
    // always did, so every existing path (the card, a comparative, a handover) is unchanged. The rest live here.
    // Each is an ordinary buildHold loop on its own slot - one window, one write to ITS OWN OUT, then it ends -
    // so the arithmetic is the loop's, not a second copy of it.
    //
    // KEYED BY THE TARGET, and held in this process: the Link sidecar carries exactly ONE `calib`, so a companion
    // cannot ride a handover. That is why the multi-hold is a V2-hosted thing today - the V2's own rack, and a
    // borrowed Link rack while the lease is here. A Link that takes a rack back mid-build keeps the primary and
    // loses the companions; making the sidecar carry an array is a LINK change and is not in this build.
    std::map<juce::String, std::vector<echojay::CalibLoop>> calibExtra_;
    // The one line the whole build closes with, composed once every hold has finished.
    std::map<juce::String, juce::String> calibHeldNames_;
    // (o), second ruling: THE DEAD LIST ACCUMULATES PER BUILD. It was local to each sweep, so setLoopsAlive
    // overwrote it every window and a hold that ended earlier lost its name on the next one. Reset when a build
    // starts (calibStartMany), added to as each hold ends, and never cleared by a window that killed nothing.
    std::map<juce::String, juce::StringArray> calibDead_;
    juce::String calibLastLogLine_;   // 21t-i: the last EJThreshold window line, for the guard and for diagnostics
    double calibLastWindowMs_ = 0.0;
    bool  borrowPushInFlight_ = false;
    int   borrowPendingCount() const { return (int) borrowPendingPushes_.size(); }
    void  borrowWriteHeadPush();                                  // writes the head of the queue (if any) and starts its ack poll
    // item 1 (12 Oct 2026): apply an ACKED set op's settings to the mirror, so the V2's dials read the Link's
    // own values rather than the mirror's stale ones (Sean's test 9: 8 ms / 150 ms sent, 50 / 50 still shown).
    void  borrowMirrorFollowAckedOps(const juce::var& editOps);
    void  borrowPollPushAck(int seq, const juce::String& id);     // 250 ms poll; ok -> pop + rebase + write the next
    // deselect/close: re-send the pending ops synchronously (bounded), then continue with `then(allAcked)`
    // R2: 5 s, ruled 25 Sep 2026 - the old 8 s was a wait nobody asked for and the user sat through it at every
    // deselect. A shorter bound is only honest because the queue now survives the release.
    void  borrowFlushPendingThen(std::function<void(bool)> then, int maxWaitMs = 5000);
    /** R2: re-arm this rack's parked edits as the session's pending queue. Returns how many. */
    int   borrowRearmParkedFor(const juce::String& uid);

    // ---- 21t-d: the compressor calibration loop --------------------------------------------------------------
    // The loop lives where the per-slot tallies are: this processor's own ChainHost for the mix bus, its
    // borrowHost while a Link's rack is leased. The Link runs the same header on its own chain after deselect,
    // and the state crosses on the sidecar.
    /** Start (or re-target) the loop on a dynamics slot. uid empty = this instance's own rack. */
    void calibStart(const juce::String& uid, int slot, const juce::String& pluginName,
                    float bandLo, float bandHi, float openingDrive,
                    echojay::CalibLoop::Purpose purpose);
    /** 21t-g item 2: start from the response's calibration block, which says the MODE (passive unless it says
        otherwise), the ACTUATOR and, for a threshold, the control name(s), the sense and the range. The old
        overload above is the listen/drive case and stays for the callers that mean exactly that. */
    void calibStart(const juce::String& uid, const echojay::CalibLoop::Config& cfg);
    /** (i) 30 Sep 2026: ONE HOLD PER COMPRESSOR. cfgs[0] is the primary and goes where the single block always
        went; the rest become companion buildHold loops on their own slots. Returns how many holds are running.
        A single-element list behaves exactly as calibStart does, so the fallback path is not a second code path. */
    int  calibStartMany(const juce::String& uid, const std::vector<echojay::CalibLoop::Config>& cfgs);
    /** The companions, for the guard and for the closing line. */
    std::vector<echojay::CalibLoop> calibExtraFor(const juce::String& uid) const
    { const auto it = calibExtra_.find(uid); return it == calibExtra_.end() ? std::vector<echojay::CalibLoop>{} : it->second; }
    /** One tick: advance the loop by any 3 s windows the owning host has measured. Returns the card text. */
    /** 21t-m item 3: true when the target chain is a BUS, with the reason. A bus dials nothing, holds nothing
        and has no slot written to it. Gated HERE rather than in the editor, because calibStart is the only door
        into a loop and calibTick is the only door to a write. */
    bool calibTargetIsBus(const juce::String& uid, juce::String& whyNot) const;
    juce::String calibTick(const juce::String& uid);
    // 21t-m (29 Sep 2026 ruling): A TEST-ONLY CLOCK FOR THE WINDOW RATE LIMIT. calibTick takes one decision per
    // 3 s window and the window is WALL CLOCK, so an end-to-end harness that feeds three seconds of audio in a
    // fifth of a second must otherwise sit out the difference - five cases at real time is minutes on every fast
    // gate. When this is set, calibTick reads it instead of juce::Time::getMillisecondCounterHiRes(); null (the
    // default, and the only state a shipping session is ever in) is the real clock, unchanged. The rate limit
    // ITSELF is still proved at real time by its own case in level_loop_guard.
    std::function<double()> calibClockMsForTest;
    /** The closing message, handed over exactly once - whoever asks first posts it. */
    juce::String calibTakeClosing(const juce::String& uid);
    // 21t-i/21t-j: the loop's one chat line, handed out exactly once. replacesOpeningOut says whether it REPLACES
    // the opening line in place (the settle completing) rather than being a new message.
    juce::String calibTakeAsk(const juce::String& uid, bool* replacesOpeningOut = nullptr);
    /** 6 Oct 2026 (Sean's ruling for 06a): THE BLIND R2 REPLAY IS OFF, so an edit the Link never confirmed is
        dropped at release - and the user is TOLD, because an edit that silently vanished is worse than one that
        failed loudly. One line, handed over exactly once, drained where the loop's own line is. The replay comes
        back next round with identity (fingerprint + name) and a satisfied check in front of it; replaying an
        index-only op against a rack that may have moved is how a "remove 3" lands on the wrong plugin. */
    juce::String takeBorrowParkedNotice();
    /** 21t-j (28 Sep 2026 ruling): CANCEL A PENDING SETTLE on that slot - any user or chat edit to it, or the
        plugin being removed or replaced. The line closes with the setting as it stands; it does not vanish and it
        does not keep promising to land something nobody is landing. Returns true if a settle was pending. */
    bool calibCancelSettle(const juce::String& uid, int slot, const juce::String& why);
    // 21t-i: the last window line this process emitted, kept as well as logged. A guard that reads the rolling log
    // FILE is asserting about the environment's logging as much as about the product - under the suite's sandbox
    // seal the file was unreadable and a judged, logged window looked identical to a silent one. The line itself is
    // the ruled behaviour, so the line is observable.
    juce::String calibLastLogLine() const { return calibLastLogLine_; }

    // ---- the stored level record (21t-i) -------------------------------------------------------------------
    // uid empty = this instance's own channel (the mix bus). The getter NEVER touches the live frame: a caller
    // that wants the frame asks for the frame.
    echojay::LevelRecord levelRecordFor(const juce::String& uid) const;
    void updateLevelRecordFromFrame(const juce::String& uid, const LinkMeterFrame& f);
    void updateOwnLevelRecord();                             // from this instance's own chain-in tally
    // THE ONLY thing that clears one: an explicit user reset, for ONE channel. Returns true if a record went.
    bool resetLevelRecord(const juce::String& uid);
    juce::var levelRecordsToVar() const;                     // for getStateInformation
    void levelRecordsFromVar(const juce::var& v);            // for setStateInformation
    void fillCalibWindow(ChainHost* host, echojay::CalibLoop& loop, echojay::CalibLoop::Window& w);
    void applyCalibStep(const juce::String& uid, ChainHost* host, echojay::CalibLoop& loop,
                        const echojay::CalibLoop::Step& step);
    void advanceCalibCompanions(const juce::String& uid, ChainHost* host, double sinceMs);
    void calibSweepCompanionsOnly(const juce::String& uid);   // (o): companions run whatever the primary is doing
    void stampCompProfileOnLoop(const juce::String& uid, echojay::CalibLoop& loop);   // spec items 3/4, flagged
    /** 4 Oct 2026: re-read the profile's in_at_gr_dbfs point-1 ladder at `norm`. Called on every stamp and after
        every amount write, because the 1 dB point moves with the control. Never called for a block-stated figure. */
    void rederiveInAtGr1(echojay::CalibLoop& loop, const juce::var& prof, float norm, const char* why);
    echojay::CalibLoop calibLoad(const juce::String& uid) const;
    void               calibStore(const juce::String& uid, const echojay::CalibLoop& loop);
    double             calibStallLogMs_ = 0.0;   // 21t-i: rate limit for the "cannot be advanced" line
    void borrowEditorClosed();
    // ---- ITEM 5 (Sean, 13 Oct 2026): THE WORKING TARGET SURVIVES THE WINDOW ---------------------------
    // Reopening the V2 showed "Released - Aitch_4_01 owns its rack again" and the V2's own empty rack. The
    // release is correct and deliberate (ruling 4: a close commits like a deselect and never strands a
    // lock) - what was missing is that nothing remembered WHICH rack you had been working on, so the
    // reopened window could only show its own. Kept on the PROCESSOR because it has to outlive the editor,
    // which is the whole point, and written into the saved state as an additive field so it also survives
    // a project reopen. Empty = this instance's own rack, the same meaning it has everywhere else.
    juce::String lastWorkingUid() const { return lastWorkingUid_; }
    void setLastWorkingUid (const juce::String& uid) { lastWorkingUid_ = uid; }
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
    // ---- ITEM 4 (Sean, 13 Oct 2026): A TURN IN FLIGHT BELONGS TO THE PROCESSOR -------------------------
    // The editor's destructor called activeChatStream_->cancel(), which killed the socket - so closing the
    // window mid-reply threw the turn away, on a Link rack and on the V2's own rack alike. 12b's
    // onChatTurnFinalised hook was built to catch exactly this and could never fire, because the stream was
    // destroyed before it could finish: I had built the catcher and left the thing that drops it.
    //
    // The editor now HANDS the live handle over instead of cancelling it. Every stream callback is already
    // guarded by a SafePointer to the editor and no-ops when it is gone, so nothing paints into a dead
    // window; the socket simply runs to completion, api.notifyChatTurnFinalised fires with no editor
    // attached, and the reply lands in orphanedChatTurns_ for the next window to drain.
    //
    // A vector because a second turn can be started and closed before the first finishes. They are dropped
    // when finished or when the processor dies - whichever comes first - and the processor outliving its
    // editor is the normal case, not the exception.
    std::vector<std::shared_ptr<ChatStreamHandle>> parkedChatStreams_;
    // The chat ids of the turns that were in the air when their window closed. Needed because the
    // onChatTurnFinalised hook keeps a reply only when NO editor is attached - and a user who closes the
    // window and reopens it before the answer arrives has an editor attached again, which would have
    // dropped the reply for the second time, by the very rule written to save it. A parked turn's chat id
    // is therefore kept and the hook honours it whether a window is open or not.
    juce::StringArray parkedChatIds_;
    /** Take over a chat stream from a dying editor so the turn can finish (item 4). */
    void parkChatStream (std::shared_ptr<ChatStreamHandle> h);
    /** Was this chat's turn parked by a closing window? (item 4) */
    bool chatTurnWasParked (const juce::String& chatId) const { return parkedChatIds_.contains (chatId); }
    void clearParkedChatId (const juce::String& chatId) { parkedChatIds_.removeString (chatId); }
    /** FIX 6, SECOND HALF (15 Oct, found by the rig's C11 row): PARK A NON-STREAMED TURN TOO.
        parkedChatIds_ was only ever filled by a parked STREAMING handle, so a turn that went down
        /api/chat - which is what a chain-shaped question does - was never marked as orphaned. And
        chatTurnOwner_ is a SINGLE pointer: the reopened window sent its own turn, overwrote it, and the
        finalise hook then read "the owner is live" about a DIFFERENT turn and dropped the first one.
        That is this morning's fault exactly ("the test is not whether an editor is attached but whether the
        SENDER is still here") with the sender's record overwritten instead of missing. Parking the chat id
        at teardown is what makes the answer survive either transport. */
    void parkChatTurnId (const juce::String& chatId)
    {
        const auto id = chatId.trim();
        if (id.isNotEmpty() && ! parkedChatIds_.contains (id)) parkedChatIds_.add (id);
    }

    juce::String lastWorkingUid_;      // item 5: the rack the editor was working on when it last closed
    juce::String borrowStickyBanner_;
    /** ---- THE RACK THE BANNER IS ABOUT (his 11:54, 21 Oct 2026) -----------------------------
        *"Undo all on the mix bus refilled it ... and the centre said "Applied to 88_7even
        Kick_2_01"."*

        The banner carried no rack, and the panel painted it over whatever rack was in view - so a
        sentence about the Kick's borrow session sat in the middle of the mix bus, directly under an
        undo he had just run there. It reads as "the undo was applied to the Kick", which is the one
        thing a user must never have to wonder about.

        Fix 4(c) of 14 Oct met this same class ("Sean was on the Mix Bus and read 'Released -
        Aitch_4_01 owns its rack again'") and answered it with a GRACE WINDOW - which hides a
        mismatched banner for a second and a half and then shows it anyway. The fact it needed is
        this one: which rack the sentence is about. Empty = about no particular rack (the solo
        banner), and those still show anywhere. */
    juce::String borrowStickyBannerUid_;
    // FIX 4(c), 14 Oct: when this banner became visible, and how long it must survive before it is SHOWN.
    // Only the release path sets a grace (see borrowApplyAndRelease); every other banner is immediate, so
    // the default of 0 keeps them all exactly as they were.
    juce::int64 borrowStickyBannerAtMs_ = 0;
    int         borrowStickyBannerGraceMs_ = 0;
public:
    /** The banner as the UI should SHOW it: empty while a graced one is still inside its grace, so a
        release that is immediately superseded by a re-engage is never seen. */
    /** The rack this banner belongs to, or empty for "any rack". The panel paints the banner only
        when this is empty or is the rack in view (his 11:54). */
    juce::String borrowStickyBannerUid() const { return borrowStickyBannerUid_; }
    juce::String borrowStickyBannerVisible() const
    {
        if (borrowStickyBanner_.isEmpty() || borrowStickyBannerGraceMs_ <= 0) return borrowStickyBanner_;
        return (juce::Time::currentTimeMillis() - borrowStickyBannerAtMs_ >= borrowStickyBannerGraceMs_)
             ? borrowStickyBanner_ : juce::String();
    }
private:
public:
    // ---- 14 OCT 2026 (the rig's first catch): WHO ASKED FOR THE TURN IN FLIGHT --------------------------
    // Set by the editor when a chat turn goes out, cleared by its destructor. onChatTurnFinalised keeps a
    // reply unless THIS pointer is the editor attached now - "an editor is attached" was the wrong test and
    // dropped a reply that landed after a reopen. A bare pointer and never dereferenced: it is only ever
    // compared with getActiveEditor(), so a stale value cannot be followed.
    // THE RIG'S COMMAND MEMORY LIVES HERE, NOT ON THE EDITOR (14 Oct 2026). EJRigHook.h promises that "a
    // command file left on disk is not re-executed on the next editor" - and it was, because the sequence
    // was an editor member that started at 0 again. Every reopen therefore re-ran the last command, which is
    // how a capability row ended up reading a rack that a replayed command had changed under it. The
    // processor outlives the window, so the memory belongs to the processor.
    int  rigLastSeq() const noexcept { return rigLastSeq_; }
    void setRigLastSeq (int s) noexcept { rigLastSeq_ = s; }

    // ---- 15 OCT 2026: AN ADDRESS IS NOT AN IDENTITY ----------------------------------------------------
    //
    // This was a raw pointer compared with getActiveEditor(), and the rig caught it being WRONG in the
    // safest-looking way there is: the editor closed, a new one was built, and the allocator handed the new
    // one THE SAME ADDRESS (0x160008000 in the run that found it). So "the window that asked is still here"
    // was true about a different window, and a failed turn was handed to a window that had never sent it.
    // A recycled address impersonating the sender is the third costume this one bug has worn ("is an editor
    // attached" -> "is the SENDER still here" -> "is this the same OBJECT or just the same place").
    //
    // So identity is a COUNTER, not a place: every editor takes the next id as it is built, and no id is
    // ever issued twice in the life of this processor. The pointer is kept beside it only for the log line.
    juce::uint64 nextEditorId() noexcept { return ++editorIdCounter_; }
    juce::uint64 editorIdCounter() const noexcept { return editorIdCounter_; }
    void setChatTurnOwner (juce::uint64 editorId) noexcept { chatTurnOwnerId_ = editorId; }
    void clearChatTurnOwnerIf (juce::uint64 editorId) noexcept { if (chatTurnOwnerId_ == editorId) chatTurnOwnerId_ = 0; }
    bool chatTurnOwnerIs (juce::uint64 editorId) const noexcept { return chatTurnOwnerId_ != 0 && chatTurnOwnerId_ == editorId; }
    juce::uint64 chatTurnOwnerId() const noexcept { return chatTurnOwnerId_; }
    /** The id of the editor attached right now, or 0 when there is none. Defined in the .cpp because it
        has to see EchoJayEditor; the keep-rule above is the only caller that matters. */
    juce::uint64 liveEditorId() const;
    /** The processor's own rig line, written on its 1 Hz timer so a row can assert on a SHUT window. */
    void publishRigProcessorState();
    /** SEAN'S #1 (16 Oct 2026): serve the parameter maps a Link asked for. A Link has no network client,
        so a third-party slot on one could never ask for the map it needs to be dialled. On the processor
        rather than the editor because a run can be going with no window attached. */
    void serviceLinkMapRequests();
private:
    juce::uint64 editorIdCounter_ = 0;
    juce::uint64 chatTurnOwnerId_ = 0;
    int   rigLastSeq_ = 0;
public:
    // ---- THE HOST RIG (Sean's ask A, 13 Oct 2026) ----------------------------------------------------
    // Two read-only windows the rig asserts through. They expose nothing a user cannot see on screen, and
    // they are plain accessors rather than a second source of truth: the banner is the member above, and
    // the slot names come from the sidecar the Link itself publishes - the same read the agent's linkRack
    // source makes, so the rig and the agent cannot disagree about what is on a rack.
    juce::String borrowStickyBannerForRig() const { return borrowStickyBannerVisible(); }   // fix 4(c): the rig sees what the USER sees
    /** Slot names on a Link's rack, straight from that Link's sidecar. Returns the count, or -1 when the
        rack cannot be read at all (no shared dir, no sidecar) - which is NOT the same as 0 slots, and the
        rig asserts on the difference. */
    int linkRackSlotNamesForRig (const juce::String& uid, juce::StringArray& namesOut) const
    {
        namesOut.clear();
        int err = 0;
        const auto dir = LinkShm::resolveDir (err);
        if (dir.isEmpty() || uid.isEmpty()) return -1;
        const auto rc = LinkShm::readRackSidecar (dir, uid);
        if (! rc.valid) return -1;
        for (const auto& sl : rc.slots) namesOut.add (sl.name);
        return namesOut.size();
    }
    /** ---- SEAN'S #1 (16 Oct 2026): THE LANDED VALUES ON A LINK'S RACK, FOR THE RIG ----------------
        "slot 1 attack_db=6;sustain_db=-2|slot 2 ..." - the same `landed` field the agent's read-back
        reads, from the same sidecar, so the rig cannot pass a row the agent would fail (or the other
        way round). Empty for a slot that read nothing back, which is the state his four plugins were
        in and the whole subject of row T1. */
    /** The same, for the slots' CARDS (16 Oct 2026, Sean's step 4 on the Link). The row that asserts
        "the card's figure is the plugin's figure" needs both halves from the same place, and the
        mirror's card travels in the sidecar's `settings`. Newlines are flattened so one slot stays
        one field. */
    /** ---- ONE SLOT'S FACTS, TOGETHER (17 Oct 2026) -------------------------------------------
        The Link's slots as an ARRAY - {n, name, landed, settings} - instead of two joined strings.

        I9c failed three runs in a row on a comparison between DIFFERENT slots: it read the ledger
        out of a joined "slot 1 ...|slot 2 ..." string by searching for the control's name, and read
        the card out of another, so with three Renaissance Axx on one rack it compared slot 1's -30
        with slot 3's card. The numbers were both right and the row was wrong. A row cannot make that
        mistake against this. */
    juce::var linkRackSlotsForRig (const juce::String& uid) const
    {
        juce::Array<juce::var> out;
        int err = 0;
        const auto dir = LinkShm::resolveDir (err);
        if (dir.isEmpty() || uid.isEmpty()) return juce::var (out);
        const auto rc = LinkShm::readRackSidecar (dir, uid);
        if (! rc.valid) return juce::var (out);
        for (int i = 0; i < (int) rc.slots.size(); ++i)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("n", i + 1);
            o->setProperty ("name", rc.slots[(size_t) i].name);
            o->setProperty ("landed", rc.slots[(size_t) i].landedValues);
            o->setProperty ("settings", rc.slots[(size_t) i].settings.replaceCharacter ('\n', ' '));
            o->setProperty ("dialPending", rc.slots[(size_t) i].dialPending);
            out.add (juce::var (o));
        }
        return juce::var (out);
    }
    juce::String linkRackSettingsForRig (const juce::String& uid) const
    {
        int err = 0;
        const auto dir = LinkShm::resolveDir (err);
        if (dir.isEmpty() || uid.isEmpty()) return {};
        const auto rc = LinkShm::readRackSidecar (dir, uid);
        if (! rc.valid) return {};
        juce::StringArray out;
        for (int i = 0; i < (int) rc.slots.size(); ++i)
            out.add ("slot " + juce::String (i + 1) + " "
                     + rc.slots[(size_t) i].settings.replaceCharacter ('\n', ' '));
        return out.joinIntoString ("|");
    }
    juce::String linkRackLandedForRig (const juce::String& uid) const
    {
        int err = 0;
        const auto dir = LinkShm::resolveDir (err);
        if (dir.isEmpty() || uid.isEmpty()) return {};
        const auto rc = LinkShm::readRackSidecar (dir, uid);
        if (! rc.valid) return {};
        juce::StringArray out;
        for (int i = 0; i < (int) rc.slots.size(); ++i)
            out.add ("slot " + juce::String (i + 1) + " " + rc.slots[(size_t) i].landedValues);
        return out.joinIntoString ("|");
    }
    // NO ACCESS TOGGLE HERE. The nearest specifier above this point is the `public:` at line 204, so these
    // accessors are already public and adding `public:`/`private:` around them would make every member
    // BELOW private - which is exactly what it did on the first attempt, and what it did once before on
    // 13 Oct. Check the nearest preceding specifier before inserting one, every time.
    bool borrowBannerIsStatus_ = false;
    int  borrowInjDiagDivider_ = 0;
    int   borrowConsumeDiagDivider_ = 0;   // 1 Hz diagnostic of the ring consume (audio thread counter)
    float borrowConsumePeakIn_ = 0.0f;         // 1 Hz diagnostic of the injection terms (audio thread counter)   // BUILD E: a held/waiting state is STATUS (calm styling), not an error
    std::map<juce::String, juce::String> unwrittenEditNote_;  // uid -> note
    // A deselect-apply FAILED and ruling 3 kept the session engaged: the
    // hold is now a pending edit, not a selection. Rides the lease
    // (additive "editPending") so the Link's lock banner can say so
    // instead of instructing a deselect the user already performed.
    bool borrowEditPendingHeld_ = false;
    juce::String pendingAutoEngage_;   // engage this uid once released
    juce::String rackLockHeldUidForInstrument() const { return rackLockHeldUid_; }   // 9 Sep: the editor's lock instrument reads this
    // ---- 11 Oct 2026, THE REMOVALS -------------------------------------------------------------------
    // borrowInContextOk_, borrowChainLat_, borrowMuteUnconfirmedTicks_, borrowLastPadKey_,
    // borrowBudgetActive_/Wanted_ and setBorrowBudgetWanted went with the injection. Round 53's ruling is
    // the one worth remembering, because it is about PDC and PDC has not changed: a plugin's reported
    // latency may only move at a moment the user cannot hear, which is why the budget committed in exactly
    // two places. There is no longer a reported figure that depends on which racks are present, so there is
    // no moment to choose.
    std::atomic<bool> capableLinkSeen_ { false };
    void markCapableLinkSeen() noexcept
    { capableLinkSeen_.store(true, std::memory_order_relaxed); }
    bool capableLinkSeen() const noexcept { return capableLinkSeen_.load(std::memory_order_relaxed); }
    // C4, the scoping decision as a pure function (the suite drives it):
    // a rack counts only if it announces in-context capability, its host
    // identity is OURS (pid + process start), and its publisher is alive.
    struct BudgetRow { bool inContextCapable = false; int publisherPid = 0; int hostPid = 0; juce::int64 hostStartSec = 0, hostStartUsec = 0; };
    static bool budgetRowCounts(const BudgetRow& r, const ChainHost::HostIdentity& me, bool publisherAlive) noexcept
    {
        return r.inContextCapable && r.publisherPid > 0 && publisherAlive
            && r.hostPid == me.pid && r.hostStartSec == me.startSec && r.hostStartUsec == me.startUsec;
    }
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
    // ---- 11 Oct 2026, THE REMOVALS: THE INJECTION'S STATE ---------------------------------------------
    // Gone with it: borrowSoloSuppressInj_ (the honorary strip), the ALIGNMENT TRACE pair, the stamp-based
    // age measurement, the no-stamps fallback flag, the mute-confirm latch and borrowCtxMixNow().
    //
    // The 28 Aug investigation is the part worth carrying forward, because it is a method rather than a
    // mechanism: the harness proved the arithmetic sample-exact at three buffer sizes, which is how the
    // remaining error was identified as TIMELINE SKEW between the Link's blocks and ours rather than a
    // mistake in the sums - and the 29 Aug ruling that followed ("the age is a MEASURED FACT, never an
    // assumed cushion; a constant true on paper and absent in the ring is how the -1024 shipped") applies to
    // anything this codebase ever aligns again.
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
    // 10 Sep 2026: kept ALSO carries the AI suggestions (prose + structured),
    // so switching racks and back does not lose the last suggestions - they are
    // kept per Link uid and restored on re-borrow, parallel to the plugin state.
    struct BorrowKept { juce::String uid; juce::StringArray names, states, settings; juce::Array<juce::var> structured; };
    // COMMIT 1 (17 Sep 2026): PER-UID, not one slot. The single borrowKept_ was
    // replaced on every release, so select A -> keep -> select B -> keep ->
    // select A found B and dropped A's suggestions. Now a session-scoped map
    // keyed by the Link's rack uid, bounded to kBorrowKeptMax (the LEAST
    // RECENTLY KEPT uid is evicted), cleared for a uid when: its kept block is
    // consumed on re-borrow (as before), the session ends clean / applied /
    // committed (as before), its chain is (re)built (sendChainToLink), or its
    // Link disappears from the registry. What is kept is unchanged.
    static constexpr int kBorrowKeptMax = 16;
    std::map<juce::String, BorrowKept> borrowKeptByUid_;
    // 17 Sep 2026 ("No suggested settings" after a rack switch): the PER-SLOT
    // DIAL INFO (suggested text, applied summary, status, manual/readback
    // lists) kept per rack uid BESIDE the kept block - captured at EVERY
    // release (the kept block is captured only on keepEdits and is CONSUMED
    // by the restore, and the applied path clears it, so a rebuilt-and-applied
    // rack came back from the sidecar with no text at all), restored onto the
    // re-created slots by identity (pluginId + index), cleared only when that
    // rack's chain is rebuilt (sendChainToLink) or the Link disappears
    // (refreshLinkRegistry). Same map, same bounds as the kept block.
    struct BorrowDialKeep { juce::String uid; std::vector<ChainHost::SlotDialSnapshot> slots; };
    std::map<juce::String, BorrowDialKeep> borrowDialByUid_;
    std::vector<juce::String> borrowDialOrder_;
    void captureBorrowDial();
    int  restoreBorrowDial(const juce::String& uid, ChainHost& bh);
    void clearBorrowDial(const juce::String& uid);
    const BorrowDialKeep* borrowDialFor(const juce::String& uid) const;
    int  borrowDialCount() const noexcept { return (int) borrowDialByUid_.size(); }
    std::vector<juce::String>          borrowKeptOrder_;   // oldest first
    void captureBorrowKept();
    void clearBorrowKept(const juce::String& uid);
    void clearBorrowKept() { clearBorrowKept(borrowSession_.uid); }   // this session's rack
    const BorrowKept* borrowKeptFor(const juce::String& uid) const;
    int  borrowKeptCount() const noexcept { return (int) borrowKeptByUid_.size(); }
    // The restore halves, moved out of the editor verbatim so the editor and
    // a headless harness call ONE implementation against the borrowed host
    // (the object that processes audio). Settings = prose + structured;
    // States = plugin state, and it CONSUMES the uid's block (as before).
    bool applyBorrowKeptSettings(const juce::String& uid, ChainHost& bh, int want);
    bool applyBorrowKeptStates  (const juce::String& uid, ChainHost& bh, int want);

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
    // 4 Oct 2026 (save fix): the Link's own chain revision as it stood when this borrow engaged. -1 = unknown
    // (no sidecar), which makes a saved copy non-authoritative on restore. See getStateInformation's
    // borrowedRacks key and the restore tie-break.
    int borrowBaseRev_ = -1;
    // 4 Oct 2026 (save fix): borrowed racks read out of the saved session and NOT yet handed to their Links.
    // Keyed by rack uid -> the saved entry {uid, baseRev, masterWet, chain}. setStateInformation only PARSES into
    // this; the hand-off happens later, on the message thread, the first time that uid is seen in the registry -
    // which is what makes the fix work in BOTH load orders (this instance before its Link, or after it).
    std::map<juce::String, juce::var> pendingBorrowRestore_;
    void applyPendingBorrowRestores();   // message thread only; called from the registry sweep
private:
    std::unique_ptr<ChainHost> borrowHost_;
    juce::SpinLock             borrowLock_;
    juce::LinearSmoothedValue<float> borrowSoloMix_ { 0.0f };
    struct BorrowLeaseTimer;
    std::unique_ptr<BorrowLeaseTimer> borrowLeaseTimer_;
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
    juce::String rackLockWantUid() const { return rackLockWantUid_; }   // COMMIT 2c: the selection tick re-arms when this is empty
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
    // 8 Sep 2026: the audio thread only SIGNALS the stop (transport stop); the message
    // thread performs it (snapshot, save thread). Serviced by the editor timer (30 Hz)
    // and the processor timer (1 Hz backstop).
    void serviceCaptureStop();
    std::atomic<bool> captureStopRequested_ { false };

    // ---- ADDITIVE SOLO (9 Sep 2026 ruling; V2-only) ----
    // On solo of Link X the main crossfades ITS OWN OUTPUT to X's ring. No Link is muted,
    // no lease is taken (the ring is written whenever the Link is on, and the main
    // already drains every ring every block), no alignment (nothing is blended: the
    // output is replaced), no PDC event. Single solo, last press wins (a set spanning a
    // feed relation would double-count and the main cannot detect one - a deliberate
    // amendment to spec 6.1's multi-solo allowance; sibling multi-solo waits for topology).
    // The crossfade sits BEFORE the main's own chain - intentional: the soloed Link is
    // heard through the mix-bus processing. This is PRE-FADER LISTEN: X's ring is its
    // insert-point signal, so fader and pan moves do not change what is heard - and the
    // panel says so. THE GUARANTEE, exactly: the main's output is X. Complete only when
    // the main is the last thing before the monitors; direct outs, hardware inserts and
    // parallel monitor paths are outside it and not detectable from inside AAX.
    // 11 Oct 2026: ONE SOURCE. The crossfade used to read the V2-hosted COPY's processed output when X
    // happened to be the rack being addressed (spec 6.1); every soloed Link is now heard from its own ring.
    // This replaced the mute broadcast, its target enumeration, the reconcile pass, the
    // authoritative mute record, the hand-mute snapshot and restore, the dead-main
    // contract, the liveness dependency for targets, the poll floor, the pending lamp
    // acks and solo-dominates-borrow: most of two days' work, deleted because the
    // measurements that produced it showed the model was wrong. The deletion is the payoff.
    void     setLinkSolo(const juce::String& uid, bool on);      // the press (message thread); on=true moves the solo to uid
    bool     linkSoloOn(const juce::String& uid) const { return uid.isNotEmpty() && uid == soloUid_; }
    bool     soloActive() const { return soloUid_.isNotEmpty(); }
    juce::String soloUid() const { return soloUid_; }
    bool     soloIndicatorOn(const juce::String& uid) const;   // ONE author for every solo indicator (sidecar flag OR the local solo)
    juce::String firstSoloName() const;
    juce::String soloUid_;                                       // message thread writes, audio thread reads the atomics below
    std::atomic<int>  soloRingSlot_ { -1 };                      // activeLinkSlots index of X's ring, -1 = none
    std::atomic<bool> soloSeekPending_ { true };                 // seek X's ring to the cushion once at engage
    juce::AudioBuffer<float> soloBuf_;                           // X's ring for this block (audio thread only)
    std::atomic<float> soloBufPeak_ { 0.0f };                    // diagnostic / leg: peak of what was consumed this block
public:
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
    // 21p item 1 (23 Sep 2026): whether the HOST has told us anything about its transport at all. A host that never
    // publishes a play head (and a harness) must not be read as "stopped" - the gate refuses a reading taken while
    // the transport is KNOWN to be stopped, and says nothing about one it cannot see.
    bool isTransportKnown() const { return transportKnown.load(); }
    LoudnessLoop& loudnessLoop() noexcept { return loudnessLoop_; }

    // Chat history — stored here so it persists when the editor is destroyed/recreated
    struct ChatEntry { 
        juce::String role, content; 
        bool hasWaveform = false;
        std::vector<float> waveform;  // simplified thumbnail (peak values)
        float durationSeconds = 0.0f;
        float lufs = -100.0f;
        juce::String wavFilename;
        juce::String wavFilePath;
        // ITEM 0 (14 Oct 2026): the AGENT RUN's snapshot (echojay::agentui::toJson), so the run row
        // survives this window being destroyed - which is the record half of item 0b. In memory and
        // deliberately NOT in getStateInformation: it is about a closed window inside one session,
        // and the run's prose is already in `content`, which is what a saved project carries.
        juce::String agentJson;
        // ---- WHICH CHAT THIS ROW BELONGS TO (Sean's 16:00, 20 Oct 2026) ----------------------
        //
        // *"The previous run's card ("Stopped. 1 change", from his earlier test) appeared in this
        // chat when he pressed Enter. This happened before too."*
        //
        // chatHistory is PER PROCESSOR, not per chat, and the reload merge in the editor puts every
        // held row the open chat's store does not have back into THAT chat. For a user turn or a
        // reply that is harmless - the store already has them, so they match and nothing is
        // inserted. For an AGENT ROW it is the bug: the row is born here with an empty `content`
        // (the prose arrives later), so it matches nothing in any store, and every chat he opens
        // looks like the chat that is missing it.
        //
        // The 11:59 fix closed the row's BIRTH into another chat ("survival is not capture"). This
        // is the other door: a row born correctly in chat A, put back into chat B by the merge.
        //
        // EMPTY MEANS "NOT STATED", and then the merge behaves exactly as it does today - which is
        // right for every row that is not an agent card, and keeps a saved project readable.
        juce::String chatId;
        /** ---- STAGE B OF THE CHAT LOG (his ruling of 21 Oct 2026) ---------------------------
            The same two facts the store's rows now carry: the entry's PLACE in its chat and its own
            IDENTITY, both set when the entry is created. `chatId` above was the first of the three
            and arrived on its own for the 16:00 fault; these two complete the set.

            Why they are here as well as in the store: this list is what a run writes into while no
            window exists, and it is what the reload used to reconcile against the store by matching
            role and content. With a place and an identity on both sides there is nothing to
            reconcile - stage C selects by chatId, sorts by seq, and updates a run's entry by id.

            In memory only, like `agentJson` above and for the same reason: this is about a closed
            window inside one session. 0 / empty = not stamped, which is every row an older build or
            an un-migrated path produced. */
        juce::int64  seq = 0;
        juce::String entryId;
    };
    std::vector<ChatEntry> chatHistory;
    bool saidUnstampedRow_ = false;   // stage B: the no-chat-id line is said once per session
    /** ---- STAGE B: ONE STAMPER FOR BOTH ROADS (his chat-log ruling) -------------------------
        The store has its own (EchoJayWorkspace::stampLogEntry) and this is the processor's, for the
        rows that reach the processor's list first - a run that outlives a window, an orphaned turn,
        a notice. It takes the chat's id and the run's key when there is one, and it asks the STORE
        for the next seq so the two roads cannot allocate the same place. `workspaceSeqFor` is the
        seam: the editor owns the workspace, so the processor is handed a reader rather than reaching
        for one (which is also what makes it testable without a store on disk). */
    std::function<juce::int64 (const juce::String& chatId)> nextChatSeq;
    /** The run's card into the stored log, by its entry id (the run's key) - an update if that run
        already has a row there, an append if it does not. Called from the stamp and from the run's
        own snapshot, so both writers take one road (22 Oct 2026, his 09:24 fault). */
    /** The stored card for a run's row, by the chat and entry id the run was minted with. Empty
        when the store has no such row. The home asks this when the replay list has been cleared by
        a chat switch - the store is the authority and the list was only ever a cache. */
    juce::String storedAgentCardJson (const juce::String& chatId, const juce::String& entryId) const;
    void storeAgentCard (const juce::String& chatId, const juce::String& entryId,
                         const juce::String& agentJson, const juce::String& content);
    void stampChatEntry (ChatEntry& e, const juce::String& chatId, const juce::String& runKey = {});
    /** ---- STAGE B: ONE DOOR INTO THE PROCESSOR'S LIST (his chat-log ruling) ----------------
        Every write to chatHistory goes through one of these two, so an entry cannot reach the list
        without its chat, its place and its id. Twenty call sites each doing it by hand is twenty
        chances to miss one, and a missed one is exactly the row stage C would have to guess about.

        A row appended with NO chat id is left unstamped and SAID ONCE in the log - that is the state
        his 19:51 fault lived in (a window whose current chat did not exist yet), and it must be
        visible before stage C relies on the field. */
    void appendChatEntry (const juce::String& role, const juce::String& content,
                          const juce::String& chatId, const juce::String& agentJson = {},
                          const juce::String& runKey = {});
    void appendChatEntry (ChatEntry e, const juce::String& chatId, const juce::String& runKey = {});
    // ITEM 3 (12 Oct 2026): turns that finished while NO editor was attached. Written by the api hook set in the
    // constructor, drained by the next editor, which renders them into the transcript and persists them to the
    // workspace. In memory only and deliberately NOT in getStateInformation: this is about a closed window
    // inside one session, and writing it into the project chunk would also make it a downgrade-safety question
    // for no gain - an older build would simply ignore the key, but the turn belongs to this session either way.
    juce::Array<juce::var> orphanedChatTurns_;
    /** Hand the waiting turns to a newly constructed editor and forget them. */
    juce::Array<juce::var> takeOrphanedChatTurns()
    { auto out = orphanedChatTurns_; orphanedChatTurns_.clearQuick(); return out; }
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
    // 21r item 5 (24 Sep 2026): a GROUP can be the target as well as a Link. Mutually exclusive with
    // chatTargetLinkUid - one selector, one answer - and carried on every turn as selectedGroupId, from which the
    // server resolves level-match and group targets (CONTRACT_GROUPS as amended 24 Sep).
    juce::String chatTargetGroupId;
    juce::String chatTargetGroupName;
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
    // REFERENCE FOLDERS, and the scope the ARROWS step within.
    //
    // On the processor rather than the editor because the scope is not browser
    // state: the reference bar's prev and next use it with the panel closed,
    // and an editor recreate (every Logic Link window switch) must not silently
    // widen it back to the whole library.
    //
    // MEMBERSHIP KEYS ON PATH because references have no ids yet. Item 137 owns
    // the migration when they do, and this is one of the places it has to
    // touch: a folder full of paths outlives a blob that has moved to ids.
    std::vector<echojay::RefFolder> referenceFolders;
    echojay::RefScope               referenceScope;

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

    /** ---- SEAN'S #3 (16 Oct 2026): THE AGENT CHANGED A RACK, SO THE VIEW MUST FOLLOW -------------
        *"after the agent adds to a Link, the main plugin's rack stays empty / 'Reading 0 plugin(s)'
        until I switch racks."*

        The 14 Oct fix (a bounded poll + a row rebuild) lives on the BUILD path, which an `add` op does
        not take: the agent's Link arm writes a chain-edit command, waits for the ack, and reports -
        and nothing then told the window that the rack it is looking at had moved. The data was right
        (the sidecar had the slot a second later) and the screen was not.

        The agent's sinks live on the PROCESSOR, which has no business knowing about panels, so it
        raises this instead. The editor sets it in its constructor and CLEARS IT FIRST in its
        destructor, the rule the hook above it records. No window attached = nothing to refresh, which
        is correct rather than a missed update: a window that opens later rebuilds its rows anyway.

        `uid` is the rack that changed ("" = this instance's own rack).

        `putAPluginIn` is TRUE when the batch ADDED or REPLACED a plugin, which is the one kind of
        agent change that alters the rack's loudness without a human at the controls - so it is also
        the one kind that re-levels (Sean's item 2, 19 Oct 2026). A Link arms its own loop from the
        command's intent; the open rack has no command to carry an intent, so it arrives here. */
    std::function<void (const juce::String& uid, bool putAPluginIn)> onAgentChangedRack;


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

    /** ---- THE PLUGINS THIS RUN MUST KEEP THE PLAYBACK FOR (B's keep-playing contract) -------
        Filled when a dial_on_playback carrying `keep_playing` is armed, cleared when a run starts.
        Read by the executor's keepPlayingPlugin source against each slot's own dialPending, so
        "still needs the playback" is two facts from two owners and never a cached guess.
        (docs/CONTRACT_keep_playing_2026-10-22.md, B to A, 22 Oct 2026 - Sean's test 9.) */
    juce::StringArray keepPlayingFor;

    /** ---- WHAT EACH DIAL FINALLY SAID, FOR THE CARD (his test 4 at 08:43, 23 Oct 2026) ------
        *"the dial-on-playback result for the Mixland Vac Attack arrived as a loose message below
         the card. Per B's keep-playing contract, the dial's result lands inside the card: the dial
         step finishes and shows its result, then the card says it's done. Nothing appears outside
         the card."*

        The calibration loop's closing sentence, kept per plugin for the run that owns that dial, so
        the card's dial step can finish WITH its result instead of vanishing and leaving the sentence
        to be posted as a chat bubble. Cleared with keepPlayingFor, once per run, in the one
        predicate both minting roads consult. Keyed by the plugin's name, which is what the dial
        step is keyed by too - one name, one row, one sentence. */
    juce::StringPairArray agentDialResults;

    /** Takes the calibration loop's closing line for `plugin` into the run's card when a run owns
        that dial, and returns TRUE when it did. FALSE means no run owns it and the line is an
        ordinary chat line, as it always was for a hand-driven dial. */
    /** Record the agent's APPENDED adds on a Link as one undo entry, so "Undo all" has something to
        walk for a run that only touched a Link (his 21:04: it had nothing and reported that as a
        failure). `slotsAfter` is the rack's size from the Link's own ack - the adds are its last
        `names.size()` slots, and the entry removes them top-first, each checked by name. */
    /** Record this run's remote adds so "Undo all" can take them back. BY NAME ONLY, no slot
        numbers: the slot each one sits on is resolved at undo time (his 09:15 rule, 24 Oct 2026). */
    void recordRemoteAddsForUndo (const juce::String& linkUid, const juce::StringArray& names);

    bool routeDialClosingToAgentCard (const juce::String& plugin, const juce::String& sentence);

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
    // 8 Sep 2026 (capture playback): the file is read AHEAD of the play position on a
    // worker; playback starts after the first second is in. abLoadedSamples_ bounds
    // what the audio thread may read. Press-to-first-sample no longer scales with length.
    std::atomic<int> abLoadedSamples_ { 0 };
    std::unique_ptr<juce::Thread> abLoader_;
    void seekAB(double seconds);
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

        /** DOES THE USER WANT THIS SLOT ROLLING? Open list 215's missing
            piece: one source of truth, so "should this be playing" stops
            being answered independently by the button, the host transport
            and the sync toggle with the loudest writer winning.

            WRITTEN BY toggleComparePlay AND BY NOTHING ELSE: true on play,
            false on pause. `playing` remains what the audio thread reads to
            advance; this is what the transport sync must consult BEFORE it
            may set `playing` true.

            THE ASYMMETRY IS THE POINT. The sync may stop a stream freely,
            because a host stopping should stop the reference. It may START
            one only where this is true, so a host rolling cannot resurrect
            something a person paused.

            APPENDED AT THE END OF THE STRUCT, NOT INSERTED. Open list 207 is
            what that rule is for: the gate links the PREVIOUS build's
            archive, so a field added mid-struct gives compiled code one
            offset table and the test TU another, silently, with plausible
            wrong answers. Every later field keeps its offset this way.

            THIS CLOSES 215 AND NOT 214. playbackPos, sampleCount and monGain
            above are still plain non-atomic members written from both
            threads. That race is still live, still intermittent, and still
            invisible to reading the code. It is deliberately NOT touched
            here: the parallel branch is already solving the same class of
            defect on the A/B path with an atomic load bound, and that shape
            should be copied after the merge rather than a second one
            invented now. */
        std::atomic<bool> userWantsRolling { false };
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
    // Codec preview mirror (set by the editor's enterCodecMode/exitCodecMode).
    // The processor cannot otherwise tell a codec render from any other file
    // in a compare slot, and the capture refusal has to NAME which feature is
    // running or it sends the user to stop the wrong thing.
    std::atomic<bool> cmpCodecPreview { false };
    // Temp buffers for muted-stream analysis (pre-allocated, avoids alloc on audio thread)
    juce::AudioBuffer<float> cmpTmpBuf;
    juce::AudioBuffer<float> cmpMixBuf;        // crossfade accumulation
    std::vector<float> cmpGainScratch;         // per-sample total monitor gain

    void loadCompareFile(int slot, const juce::String& wavPath);
    void fadeOutCompareStreams();   // ramp monitor gain to 0, streams self-stop (click-free)
    void stopCompareStream(int slot);

    /** SILENCE A SLOT WITHOUT UNLOADING IT.

        A CLOSED WINDOW IS NOT A REMOVED REFERENCE. Teardown wants the audio to
        stop; it does not want the buffer freed, the position reset or the slot
        emptied. stopCompareStream does all of that, because it is the REMOVAL
        path and should keep doing it.

        WHAT IT DOES NOT TOUCH, and each one is a way to lose something the
        user still has: `loaded`, the sample buffer, `playbackPos`, `filePath`
        and `sampleCount`. Reopening the window finds the stream exactly where
        it was.

        A FUNCTION RATHER THAN A FLAG ON stopCompareStream, so that neither
        caller can be read as the other by mistake: the two are opposite
        intentions that happen to share four lines. */
    void silenceCompareStream(int slot);

    /** WHAT THE TWO COMPARE SLOTS HELD, OUTLIVING THE EDITOR.

        TWO LIFETIMES, TWO MECHANISMS, and they are separate on purpose:
          the PROCESSOR copy survives a WINDOW close, because the processor is
            not destroyed when the editor is;
          the BLOB copy survives a PROJECT reload, because the processor is.

        [0] is the top slot and [1] the bottom, matching cmpStream. */
    echojay::CompareSlotPersist compareSlotPersist_[2];

    /** THE REFERENCE LIBRARY AS THIS INSTANCE HOLDS IT (commit C2).

        KEPT RATHER THAN REBUILT FROM THE ANALYSER, because ReferenceResult has
        no id and no addedAt. Rebuilding would mint a fresh id on every commit;
        mergeReferenceIndex would then dedupe by path and keep the earlier
        addedAt, so nothing would break, but every write would churn ids for no
        reason. Holding the reconciled index preserves them, and is what makes
        a tombstone possible at all.

        refIndexMayWrite_ is rec.mayWrite from the load. FALSE ONLY when the
        file existed and could not be read, in which case this session writes
        nothing: the file that could not be read is the user's library. */
    echojay::ReferenceIndex refLibrary_;
    bool                    refIndexMayWrite_ = true;

    /** The load result itself, kept because refReconcile takes one and because
        Absent, Loaded and Unreadable are three different things that must not
        collapse into a bool. */
    echojay::RefLoadResult  refLoaded_;
    bool                    refLibraryLoaded_ = false;

    /** LOAD AND SEED ONCE, ON WHICHEVER PATH ARRIVES FIRST.

        NOT IN THE CONSTRUCTOR, and that is the whole point of this being a
        function. A host SCAN instantiates every plugin it finds, so a parse in
        the constructor is a 473 KB JSON read on every scan of every plugin,
        for a scan that never looks at a reference. setStateInformation and
        createEditor are the two paths that actually need the library, and a
        scan calls neither.

        IT USED TO LIVE INSIDE setStateInformation, which a host calls ONLY
        when restoring saved state. A freshly inserted plugin has no state, so
        the load never ran and the library was never there: the exact case the
        work exists to fix. */
    void ensureReferenceLibraryLoaded();

    /** Where the index lives. ONE definition, because EJReferenceIndex.h
        forbids the header resolving it and two call sites computing it
        separately is how they drift. */
    static juce::File referenceIndexDir()
    {
        // THROUGH THE STATE ROOT, NEVER A RAW ~/Library. This arrived with the reference library on the
        // integration line and went straight to juce::userApplicationDataDirectory, which no runner can
        // isolate: under a guard it reads and WRITES Sean's real library. echojay::userAppData() is the same
        // path in a normal run and $ECHOJAY_STATE_HOME/Library under a harness. Caught by state_path_guard.
        return echojay::userAppData().getChildFile ("EchoJay");
    }

    /** Write the library, merged, after a change. Message thread, no lock. */
    void commitReferenceLibrary (const juce::String& path, bool removed);

    // ---- FOLDERS (C3b). THE INDEX IS THE STORE OF RECORD -----------------
    //
    // referenceFolders BECOMES A DERIVED VIEW. The browser, the scope and the
    // menus all read it by NAME and there are dozens of those reads in
    // PluginEditor.cpp, which is where the unmerged parallel work is heaviest.
    // Rebuilding it from the index after every change means none of them move:
    // the editor's four folder functions each gain ONE line and nothing else
    // in that file changes.
    void refreshFolderView();

    /** THE FOUR OPERATIONS, each addressed the way the editor already has it
        (by name) and translated to ids here, because 4A.3 requires the index to
        key on id and the editor does not have one. */
    void folderCreate (const juce::String& name);
    void folderRename (const juce::String& oldName, const juce::String& newName);
    void folderDelete (const juce::String& name);
    void folderAssign (const juce::String& path, const juce::String& folderName);

    /** THE ONE TIME MIGRATION (schema 4A.5). Blob folders become index folders,
        ids minted now, order from array position, each path becoming that
        entry's folderId with the first match winning. */
    void migrateBlobFoldersOnce();
    bool refFoldersMigrated_ = false;
    void stopAllCompare();

private:
    // THE PROCESSOR'S LIVENESS TOKEN. Every deferred callback that would touch this
    // object - the §5a-R ack poll, and (26 Sep 2026) the state-restore callAsyncs -
    // holds it WEAKLY instead of `this`, and goes inert the moment the destructor runs.
    // Renamed from aliveToken_ when the restore path started using it: it is not
    // the borrow's token, it is the processor's, and the old name made three raw-`this`
    // captures look like a different concern. See ~EchoJayProcessor.
    std::shared_ptr<bool> aliveToken_ { std::make_shared<bool>(true) };
    // The producer's rings, one per rack uid, mapped lazily on the message thread and kept for the session.
    // A std::map is fine here: this is never touched from the audio thread - the V2 PRODUCES, it does not drain.
    std::map<juce::String, echojay::cmdring::CmdRing*> cmdRings_;
    std::atomic<uint32_t> ringGestureSeq_ { 0 };   // gesture ids, and the frame seq, both strictly increasing
    echojay::cmdring::CmdRing* ringFor (const juce::String& linkUid);
    bool ringPushFrame (const juce::String& linkUid, uint32_t phase, uint32_t gesture, int kind, int slot,
                        int paramId, double value, uint32_t structureRevision);
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
    echojay::UndoHistory undoHistory_;   // 21n item 3
    juce::String lastUndoStatus_;
    struct GestureTimer : juce::Timer { EchoJayProcessor& p; explicit GestureTimer(EchoJayProcessor& pp) : p(pp) { startTimerHz(20); } void timerCallback() override { p.drainHostedGestures(); } };
    std::unique_ptr<GestureTimer> gestureTimer_;
    void drainHostedGestures();
    LoudnessLoop loudnessLoop_ { chainHost };   // 18 Sep 2026 ruling G: the interactive loudness closed loop (declared after chainHost)
    // Declared AFTER chainHost and before nothing that uses it at
    // construction. See getApi() above for the lifetime argument.
    EchoJayAPI api;
    /** Declared AFTER api, chainHost and undoHistory_, which its sinks read, so it is destroyed
        BEFORE them - the same lifetime argument getApi() above makes for api itself. */
    std::unique_ptr<EJAgentHome> agentHome_;

    /** The process-wide poller. juce::SharedResourcePointer creates it on
        first use and destroys it when the last processor releases it, so the
        lifetime is refcounted rather than a leaked static. */
    juce::SharedResourcePointer<DashPollShared> dashPoll;

    ChannelType channelType { ChannelType::FullMix };
    int         selfPlacement_ = 0;   // 21t-m item 5: the Link's placement selector; 0 on V2, which has none
    // 21t-m: the music verdict, computed on the message thread by publishChainRole() and READ on the audio
    // thread. An atomic<bool> because that is the only shape this question may take in processBlock.
    std::atomic<bool> selfKeyIsMusic_ { true };
    juce::String customChannelName;
    bool channelTypePromptDismissed = false;
    bool channelChosen = false;   // 21r item 7: persisted "the question has been answered"
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
    // Stamped at startCapture (after the guard, so "" today) and copied onto
    // the snapshot at stopCapture, because a substitution can stop mid-capture
    // and what matters is what the capture BEGAN under.
    juce::String captureSubstitution_;

    // PLAYBACK SIMULATION, the whole stage: the selection (the same atomic and
    // refusal rule as before), one voicing chain per channel, the voicing the
    // previous block ran, and the room's reverb network with its own state and
    // rule (about 320 KB of delay lines at 48 kHz, allocated in prepare).
    // EJPlaybackSim.h is where the memory order and both reset rules are stated
    // and argued once. It lives there rather than here so
    // mapfps_test can exercise the shipped stage instead of a copy of it.
    // Prepared in prepareToPlay; run at the end of processBlock.
    PlaybackSimStage playbackStage_;

public:
    /** Select a playback simulation, OR REFUSE THE VALUE.

        A PAIR RATHER THAN A PUBLIC MEMBER, against this file's own convention
        of public atomics reached directly, for one reason: a setter can refuse
        a value the enum permits but the switch cannot handle. WHICH VALUES ARE
        REFUSED, AND WHY, IS ARGUED ONCE at PlaybackSimSelection in
        EJPlaybackSim.h, not restated here. Pinned by pb PIN8. */
    void setPlaybackSim (PlaybackSim s) noexcept  { playbackStage_.select (s); }

    /** The current selection. The editor draws its card state from THIS rather
        than from a bool of its own, so the button cannot show one thing while
        the audio does another. */
    PlaybackSim playbackSim() const noexcept      { return playbackStage_.selected(); }

private:

    // Auto-feedback
    mutable std::atomic<bool> autoFeedbackReady { false };

    // Silence detection (triggers auto-stop when DAW stops)
    std::atomic<bool> audioSilent { true };
    std::atomic<bool> transportPlaying { false };
    std::atomic<bool> transportKnown   { false };   // 21p item 1: set the first time a play head answers
    int latLogBlocks_ = 0;   // round 49: blocks still to log after a play start (EJ_LATENCY_LOG builds only)
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
        bool         audioFlowing = false; // 21m ruling 1 (22 Sep 2026): the ring is bound AND frames arrived since the last poll
        float        sampleRate = 0.f;
        int          channels   = 0;      // 21n ruling 1b (22 Sep 2026): the Link's channel width from its registry row (0 = unknown / old Link)
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
        // 18 Oct 2026: the Link says it is REBUILDING its rack (registry byte 126). False from every older
        // Link, and false never means "idle" - only "this writer cannot say". BUSY IS NOT DEAD: during a
        // build this plugin's heartbeat stalls, so every reader that judges reachability by the heartbeat
        // alone calls a busy rack a dead one (queue items 13, 16, 21).
        bool busy = false;
        bool heartbeatFresh = true;       // heartbeat advanced within ~3s.
        // 4 Oct 2026: seconds since the heartbeat last CLIMBED, by wall clock. -1 = never seen climbing. Use this,
        // not heartbeatFresh, for any decision about whether the Link can answer - see SlotProbeState.
        float heartbeatAgeSeconds = -1.0f;
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
    void updateLinkAudioRecency();   // 1 Hz stamp of which Links pass audio (public: also driven by the assembly harness)

    /// Read a Link's latest published meter frame (message thread). Returns
    /// false on torn read / no registry — keep the previous copy. Staleness
    /// is detected by out.seq not advancing between reads (~10Hz expected).
    bool readLinkMeterFrame(int regIdx, LinkMeterFrame& out);

    /// Last time (getMillisecondCounter) the 1 Hz timer saw this Link's frame
    /// passing audio (audioStale==0). 0 = never seen flowing. Message thread.
    /// Recency source for the Link [METER SNAPSHOT] gate; the frame carries no
    /// timestamp and the Link is unchanged, so recency is tracked here.
    juce::uint32 linkLastFlowingMs(const juce::String& uid) const;
    // LAST-GOOD METER LATCH read (15 Sep 2026): the most recent frame in which
    // this Link's short-window fields were valid, and how long ago (ms) it was
    // captured. False when nothing has ever been latched for the uid. Assembly
    // gates ageMsOut against the 5-min recency window; beyond it, Lapse fires.
    bool linkLastGoodFrame(const juce::String& uid, LinkMeterFrame& frameOut, juce::uint32& ageMsOut) const;

    /// Snapshot of currently known slots — message thread only.
    const std::vector<LinkSlotInfo>& getLinkSlotInfos() const { return linkSlotInfos; }

    // ---- CAN THIS LINK ANSWER RIGHT NOW (18 Oct 2026) --------------------------------------------------
    // ONE floor, named once. The Link heartbeats at 1 Hz, so three seconds is three missed beats: past any
    // jitter and well short of the ~6 s the reaper uses. borrowEditorClosed derived this figure on 4 Oct and
    // every decision of this shape must use the same one, measured in TIME - a count of consumer polls says
    // more about how busy the editor was than about whether the Link is alive (see heartbeatFresh).
    static constexpr float kLinkAnsweringWithinS = 3.0f;
    // How long anything here may go on waiting for a rack that SAYS it is busy and whose registry row agrees.
    // 30 s, and the figure is a judgement about PEOPLE rather than about plugins: a rack of slow AUs can take
    // longer than this to build, but a user or an agent step left with no answer for half a minute needs to be
    // told something true now - and what it is told then is that the rack was still rebuilding, which is the
    // truth and is actionable. The windows for a rack that says NOTHING are unchanged (8 s for a chain ack,
    // 5 s per slot for a borrowed read).
    //
    // ONE constant for both waits, deliberately. awaitChainAck waits on a chain command's ack and the borrowed
    // dial read waits on a ctrl-cmd pull - two channels, one question ("how long do we believe a rack that
    // says it is building"), and two numbers for one question is how they drift apart.
    static constexpr int kBusyWaitHardMaxMs = 30000;
    struct LinkAnswerability
    {
        bool  found     = false;   // a row with this uid is listed at all
        bool  connected = false;   // ...and its ring is bound (its Active role is on)
        bool  busy      = false;   // ...and it says it is rebuilding its rack
        bool  answering = false;   // ...and its heartbeat climbed within kLinkAnsweringWithinS
        float ageS      = -1.0f;   // seconds since the heartbeat last climbed; -1 = never seen climbing
    };
    /// What the REGISTRY says about a Link, for any sentence or decision about reachability. Message thread.
    LinkAnswerability linkAnswerability (const juce::String& uid) const;

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
    // 21m rename alias (22 Sep 2026): a V2-SESSION alias for a Link (by uid), shown everywhere V2 names the Link (this list is
    // the one source); the Link's own name is untouched; "Reset name" clears it. Persisted with the session state.
    // ===== 21n item 4 (22 Sep 2026): LINK GROUPS, client half (CONTRACT_GROUPS_2026-09-22.md) =====
    // A group = a name + member Link instanceIds (display order) + an optional bus Link. Persisted in the V2 session state
    // ("linkGroups"). Every chat / chat-stream / classify body carries groups[] {id, name, members, bus} beside links[]
    // {instanceId, name, gainDb} whenever at least one group exists (omitting them = today's behaviour, per the contract).
    // offsetDb (21r item 4, 24 Sep 2026): the group's own level offset, like a VCA - it applies to the members,
    // it stays where it was put, and it is saved with the group.
    // 21t-k item 1b (29 Sep 2026 ruling): A MEMBER CARRIES ITS LAST KNOWN NAME, parallel to `members`. Two
    // rulings need it and neither can be kept without it: the [GROUP LEVELS] line must read "<last known name> -
    // not in this session" instead of a bare uid, and the one-time repair for groups broken by the retired
    // re-mint rule rebinds a dead uid to the live Link carrying the same name. Written at create/set time, saved
    // with the group, and refreshed whenever the Link is live - so it follows a rename.
    struct LinkGroup { juce::String id, name, bus; juce::StringArray members, memberNames; float offsetDb = 0.0f; };
    // 21t-j (28 Sep 2026 ruling): THE CAPS, raised. Sixteen members was the wrong size for a session where "every
    // channel" is the group that matters: a 22-strip selection came back as a group of 16, silently, because the
    // store truncated. 64 members and 16 groups, on both sides (B changes the contract doc and its parser).
    // NOT a shared-memory change: groups live here, in V2's own state and on the wire; LinkShm has no group table,
    // so RegistrySlot and LinkMeterFrame keep their frozen layouts and the shm version does not move.
    // 21t-k item 1b: the one-time repair, armed by a state load and spent on the third tick after it.
    bool linkGainUndoSuppressed_ = false;   // 21t-l item 1: one entry for a whole op, not one per Link
    bool groupRepairArmed_ = false;
    int  groupRepairTicks_ = 0;
public:
    static constexpr int kMaxGroupMembers = 64;
    static constexpr int kMaxGroups       = 16;
    const std::vector<LinkGroup>& linkGroups() const { return linkGroups_; }
    /** 21t-j: `refusedOut` names the members that could NOT be taken, with the reason - a group holds exactly the
        selection, and anything it cannot hold is said out loud rather than dropped. */
    juce::String createLinkGroup(const juce::String& name, const juce::StringArray& members,
                                 const juce::String& bus = {}, juce::StringArray* refusedOut = nullptr);   // returns the id
    void removeLinkGroup(const juce::String& id);
    /** 21t-k item 4 (28 Sep 2026): THE ROSTER, counted from the same role the strip prints. `unset` is excluded
        from every scope and SAID, because a Link nobody has declared is not a channel the op can be about. */
    struct RosterCounts { int channels = 0; int buses = 0; int unset = 0; };
    RosterCounts linkRosterCounts() const;
    juce::String linkRosterSentence() const;   // "N channels, M buses, K unset"
    /** The live uids a scope selects. role "" = every DECLARED Link (channels and buses, never unset).
        `excludedOut` names what the scope left out and why, one entry per Link, for the card and the log. */
    juce::StringArray uidsForScopeRole(const juce::String& role, juce::StringArray* excludedOut = nullptr) const;
    /** 21t-k item 1b: the last known name of a member, for the line that must not print a uid. Empty when the
        group was saved before names were kept. */
    juce::String groupMemberName(const juce::String& groupId, const juce::String& memberUid) const;
    /** ONE-TIME REPAIR, on load: a member uid no live Link carries is rebound to the live Link whose name
        matches the one stored for it. Returns how many were rebound; every one is logged. Groups saved before
        names were kept have nothing to match on, and that is logged too. */
    int repairGroupsByName();
    /** 21t-l item 7 (29 Sep 2026 ruling): WHEN THE REPAIR RUNS. On the tick where the registry first shows a
        live Link, or 30 s after the load, whichever comes first - and NEVER on an empty registry, because a
        repair with nothing to bind to spends its one chance on nothing. Sean's session ran it at 10:37:28, four
        seconds into the load, before any Link had registered: "8 with a name but no live Link". A pure decision,
        so the guard reads the same rule the tick does. */
    struct RepairDecision { bool run = false; bool giveUp = false; };
    static RepairDecision groupRepairDecision (bool anyLiveLink, int ticksSinceLoad);
    static constexpr int kGroupRepairGiveUpTicks = 30;   // ruled: 30 s at 1 Hz
    void setLinkGroupBus(const juce::String& id, const juce::String& busUid);
    void setLinkGroupMembers(const juce::String& id, const juce::StringArray& members);
    void renameLinkGroup(const juce::String& id, const juce::String& name);
    void setLinkGroupOffsetDb(const juce::String& id, float db);   // 21r item 4
    const LinkGroup* linkGroupById(const juce::String& id) const;
    juce::var groupsBodyVar() const;    // groups[] as the contract shapes it (void when there are no groups)
    juce::var linksBodyVar() const;     // links[] {instanceId, name, gainDb} for a groups-aware body (void when no groups)
    // the group's level offset: each present member's trim moves by the largest |delta| in the requested direction that
    // keeps every member inside -24..+12 (the bus alone when set); returns the applied delta and names the limiting member
    struct GroupMove { float requested = 0, applied = 0; juce::String limitingMember; juce::StringArray commands; };
    GroupMove moveLinkGroup(const juce::String& id, float deltaDb, bool sendCommands = true);
    void setLinkAlias(const juce::String& uid, const juce::String& alias);
    // 21n item 2: the alias is MIRRORED to the Link over the ctrl-cmd transport ("alias" field, "" on Reset name) as
    // display-only; the Link keeps reporting its track name as identity. Public for the guard.
    void writeLinkAliasCommand(const juce::String& uid, const juce::String& alias);
    juce::String linkAlias(const juce::String& uid) const;
    std::map<juce::String, juce::String> linkAliases_;
    std::vector<LinkGroup> linkGroups_;   // 21n item 4
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
    static constexpr int kMaxLinkSlots = kRegMaxSlots;   // the registry ceiling (256 since 6 Sep 2026) - NEVER walked per block, see liveSlotIdx_
    // THE LIVE LIST (6 Sep 2026 ruling): the audio thread walks only the slots
    // the message-thread walk connected. Written by refreshLinkRegistry /
    // disconnectAllLinkSlotsNow (indices first, then the count with release);
    // a slot disconnected between walks still has map == nullptr under its lock.
    std::array<int, kRegMaxSlots> liveSlotIdx_ {};
    std::atomic<int>              liveSlotCount_ { 0 };
    void publishLiveSlotList();

    // Resolved shared directory (message thread, set once in ensureLinkRegistryOpen)
    juce::String linkResolvedDir;
    juce::int64  lastFileReapMs_ = 0;   // dead-uid file sweep throttle (~5 min)
    std::map<juce::String, BudgetRow> ctxCapCache_;   // uid -> what its sidecar said (round 53: with publisher + host)
    std::map<juce::String, juce::uint32> linkLastFlowingMs_;   // uid -> ms the 1 Hz timer last saw audioStale==0 (Link meter recency)
    // updateLinkAudioRecency() declared public above (assembly harness needs it)

    // LAST-GOOD METER LATCH (15 Sep 2026). A Link build fires at the quiet
    // moment after playback stops, when the frame's SHORT-WINDOW fields
    // (momentary/shortTerm/crest/correlation/width/macroBands) have decayed to
    // floor while the CUMULATIVE fields (integrated/true peak/LRA/PLR) still
    // hold. The 1 Hz recency poll retains, per uid, the most recent frame in
    // which the short-window fields were valid (momentary > -70 AND at least
    // one bandRel above the floor) plus the wall-clock ms it was captured, so
    // assembly can serve while-flowing short-window values with the TRUE age of
    // THAT measurement, not the age of the read. Message thread only, like
    // linkLastFlowingMs_. Dropped when a uid leaves the display list
    // (belt-and-braces: instanceUid is per-instance and re-minted on collision,
    // so a uid cannot address a different channel). Bounded: one small frame
    // per Link seen (~144 bytes), at most kMaxLinkSlots.
    struct LinkGoodFrame { LinkMeterFrame frame {}; juce::uint32 stampMs = 0; bool valid = false; };
    std::map<juce::String, LinkGoodFrame> linkLastGoodFrame_;
    // ---- THE STORED LEVEL RECORDS (21t-i, 27 Sep 2026 ruling) ----------------------------------------------
    // One record per Link uid, plus this instance's own (the mix bus, key ""). The live frame feeds the STRIPS;
    // every chat block is composed from these, which is why a quiet Link is a member with data and "no signal"
    // means HEARD 0. Kept across transport stop/start and editor close because they are members here, and across
    // a session save/reopen because they ride this plugin's state. Cleared only by an explicit user reset.
    std::map<juce::String, echojay::LevelRecord> levelRecordByUid_;
    echojay::LevelRecord ownLevelRecord_;
    // 21t-i re-cut: the uids whose OWN sidecar record we have already looked for. A Link that publishes none (an
    // older binary, or one that has heard nothing yet) would otherwise be re-read from disk once a second for the
    // life of the session - a file open per Link per tick with no reader. Tried once; the frame feed keeps working.
    std::set<juce::String> levelRecordSidecarTried_;

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
        int64_t              lastPollFrames = -1;   // 21m ruling 1: frames seen at the previous poll (message thread)
        // Non-copyable due to SpinLock — managed in-place via std::array
    };
    std::array<ActiveLinkSlot, kMaxLinkSlots> activeLinkSlots;


    // The §8 alignment gate binds a REAL ring end-to-end (producer in the
    // test, consumer here) — the one gate that asserts in-context is IN
    // TIME, not just level-safe and continuous.
    friend struct EchoJayAlignTestAccess;
    // 21t-a H3: the guard's own borrow session (no Link process exists under an isolated state home)
    friend struct EchoJayBorrowTestAccess;
    // 21t-a H1: save/restore the onboarding answers around the leg that has to dismiss them
    friend struct EchoJayPromptTestAccess;
    // 21t-c: the guard's own latched Link frame - no Link publishes under an isolated state home
    friend struct EchoJayLinkFrameTestAccess;
    void connectLinkAudioSlot   (int i, const juce::String& key, const juce::String& displayName,
                                 float sr, const juce::String& uid);
    void disconnectLinkAudioSlot(int i);
    void disconnectAllLinkSlotsNow();  // destructor

    // Stale detection (message thread)
    // 4 Oct 2026: lastHbAdvancedMs is a WALL CLOCK, and it exists because staleCycles is not one. staleCycles
    // counts CONSUMER POLLS without an advance, and refreshLinkRegistry runs both on the 1 Hz timer and from four
    // editor sites (tab switches, applies) - so a burst of user actions can run the count up while the Link is
    // perfectly alive, and anything deciding "is it answering" from the count is measuring dispatch order.
    struct SlotProbeState { uint32_t lastHb = 0; int staleCycles = 0; double lastHbAdvancedMs = 0.0;
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
