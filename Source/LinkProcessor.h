#pragma once
#include <JuceHeader.h>
#include "ChainHost.h"
#include "MeterEngine.h"
#include "EedKeyEngine.h"   // detected key -> LinkMeterFrame (KEY_DETECTOR_SPEC §9)
#include "EedKeyWorker.h"
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
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
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
    // DEFAULT Active (27 Jul): a freshly inserted Link contributes its
    // meters/ring immediately instead of needing a hand-tick on every
    // channel. DEFAULT ONLY — setStateInformation applies a persisted
    // value whenever the property exists, so a project deliberately saved
    // inactive still loads inactive. (Projects saved by builds that never
    // wrote the property adopt this default and come back active —
    // accepted.) Active gates the METER ENGINE + the shared-memory RING
    // and meter-frame publication only; the hosted chain keeps processing
    // audio, the registry heartbeat keeps the Link visible, and remote
    // control still works while inactive.
    std::atomic<bool> linkOn    { true };

    // ---- Host track name (Phase N) ----------------------------------------
    // The DAW-provided channel name. Precedence everywhere a name is shown
    // or published: user-typed linkName > hostTrackName_ > "" (the main
    // plugin renders empty as "Untitled N"). The callback never fires in
    // the constructor, arrives late or never (host-dependent), repeats on
    // renames, and the AU ContextName listener can fire OFF the message
    // thread — so writes stash under a lock + dirty flag and the 10 Hz
    // timer (message thread) applies via the established rename path
    // (updateShmState re-claim). Colour is VST3-only and deferred.
    void updateTrackProperties(const TrackProperties& props) override;
    juce::String getHostTrackName() const;      // any thread
    juce::String effectiveDisplayName() const;  // the precedence chain

    // ---- Built-in gain stage (v0.5.7) ------------------------------------
    // A single wideband gain on the signal path, applied POST-chain and
    // PRE-meter-tap (see processBlock for the justification). Range -24..+12
    // dB, smoothed to avoid zipper noise, bit-transparent at exactly 0 dB.
    // Target is an atomic set on the message thread (editor / remote cmd /
    // state restore); the audio thread reads it and glides the smoothed
    // linear gain toward it. Adds NO latency.
    static constexpr float kGainMinDb = -24.0f, kGainMaxDb = 12.0f;
    std::atomic<float> gainDb_ { 0.0f };
    float  getGainDb() const { return gainDb_.load(std::memory_order_relaxed); }
    // Message thread: clamp, store, mirror to registry slot, dirty-mark, and
    // notify an open editor. snapSmoothing = jump the smoother to the value
    // instead of gliding (used on state restore / prepare so a project load
    // doesn't swell up from 0).
    void   setGainDb(float db, bool snapSmoothing = false);

    // ---- Placement declaration (v0.6.0) ----------------------------------
    // The plugin cannot read the channel fader or reliably detect its insert
    // position under Logic (see the investigation), so the user declares it.
    // 0 = unset/unknown (treated as pre-fader for level gating), 1 = bus
    // (post-fader — loudness IS its contribution), 2 = insert (pre-fader —
    // measurements are before the fader, not comparable across channels).
    // 3 = send return: a parallel/FX bus fed by sends. POST-FADER like
    // PlacementBus for MEASUREMENT (its readings are its real
    // contribution), but it earns its own guidance in the AI context,
    // because a send that reads below the dry channel is blended, not quiet.
    enum Placement { PlacementUnset = 0, PlacementBus = 1, PlacementInsert = 2,
                     PlacementSend = 3 };
    std::atomic<int> placement_ { PlacementUnset };
    int  getPlacement() const { return placement_.load(std::memory_order_relaxed); }
    /// Stage 1 remote editing: which rack slot is externally controlled
    /// (leased to the main plugin), -1 = none. Editor reads it on rebuild.
    int controlledSlot0() const
        { return leaseActive_.load(std::memory_order_relaxed) ? leaseSlot0_ : -1; }
    void setPlacement(int p);   // message thread: store, mirror, dirty-mark, notify

    // Session project name (no UI here): adopted from the shared
    // session_project.json when this instance has none of its own, follows
    // shared edits while it matches the previous shared value, serialised
    // with state. Own restored value always wins (never overwritten).
    juce::String projectName;

    // Session genre — same adopt/follow rules, for payload context. Empty
    // until a session genre exists (Link has no genre default or UI).
    juce::String genre;
    std::atomic<bool> didWrite  { false };  // set by audio thread on first successful produce
    // Stage 0 position stamps: audio thread arms, timer logs once (NSLog is
    // not an audio-thread call). stampArmedLogged_ is message-thread only.
    std::atomic<bool> stampsArmed_ { false };
    bool stampArmedLogged_ = false;

    /// Call from editor after any change to linkOn or linkName (message thread).
    void updateShmState();

    /// Fired on the message thread when linkOn/linkName change OUTSIDE the
    /// editor (state restore, remote ctrl command) so an open editor can
    /// sync its toggle — a stale ON toggle writing itself back into the
    /// processor is a re-activation path.
    std::function<void()> onLinkStateChanged;
    /** STAGE 1 REMOTE EDITOR. The main plugin asks this Link to open one of
        its OWN hosted editors, in its OWN window. Nothing is transferred and
        nothing about the audio changes: the instance being edited is the one
        already in the signal path, so the user hears every change instantly,
        which is the whole reason this beats moving state or moving audio.

        Set by LinkEditor while it exists. NULL IS THE ANSWER, not a failure:
        no editor means the Link's window is closed, and only a window can
        show a window. The caller reports that honestly rather than silently
        doing nothing.

        Takes a 0-based rack index, returns true only if an editor was
        actually brought up. Message thread. */
    std::function<bool(int)> onOpenSlotEditor;

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
        float wet     = 1.0f;      // per-slot wet/dry (0..1), mirrors ChainHost
    };
    static constexpr int kMaxChainSlots = 15;

    struct ChainBuildItem {
        juce::String name;
        juce::String settings;
        bool bypassed = false;
        float wet     = 1.0f;      // per-slot wet/dry (restore path)
        juce::String stateBase64;  // hosted plugin state (restore path only)
        // THE FIELD THAT WAS MISSING (11 Aug 2026). settings_structured has
        // always crossed the wire -- sendChainToLink passes the chain array
        // through verbatim -- and this struct had nowhere to put it, so the
        // parse read `settings` (prose) and dropped the dial payload on the
        // floor. Channel builds have therefore NEVER dialled: the whole apply
        // path is compiled into this binary via ChainHost.cpp and nothing ever
        // handed it a value.
        juce::var structured;      // settings_structured, void when absent
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

    // Wet/dry (model-indexed like the other slot mutators). Setters are
    // knob-drag cheap (atomic writes, no host-state churn); the editor calls
    // commitChainWetChange() once on gesture end so the host re-snapshots
    // state without being spammed during the drag.
    void  setChainSlotWet(int idx, float wet01);
    float getChainSlotWet(int idx) const;
    void  setChainMasterWet(float wet01)
    {
        // Rack lock: master wet is a mix write, locked with structure.
        if (rackLockGuard("master wet")) return;
        chainHost.setMasterWet(wet01);
        stampLocalRackEdit();
    }
    float getChainMasterWet() const      { return chainHost.getMasterWet(); }
    void  commitChainWetChange();

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

    // Chain support flag — set in prepareToPlay. Mono AND stereo are supported
    // now; only exotic (>2ch) layouts are unsupported (chain bypassed).
    bool chainLayoutSupported() const { return chainSupported_.load(); }
    // True on a 1-channel track — the editor greys correlation/width to dashes.
    bool isMonoLayout() const { return chainMono_.load(); }
    // Status-line note (empty when none) shown when the mono fold-down is
    // discarding real stereo work: a hosted plugin's L and R diverge on the
    // duplicated-mono input, so take-L is dropping its right output.
    juce::String getMonoFoldNote() const;

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

    // Gain smoother — audio thread only; prepared in prepareToPlay (30 ms
    // ramp). Target set from gainDb_ each block; a pending-snap flag lets
    // the message thread request an instant jump (restore/prepare) without
    // touching the smoother off-thread.
    juce::LinearSmoothedValue<float> gainSmoothed_ { 1.0f };
    std::atomic<bool> gainSnapPending_ { true };
    bool applyGainSmoothed(juce::AudioBuffer<float>& buffer);   // false if pure unity no-op

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
    // Set in prepareToPlay (host/audio thread), read on the audio thread and by
    // the editor timer — atomic. chainSupported_ = mono OR stereo in/out;
    // chainMono_ = a 1-channel track (needs the stereo up-mix / fold-down).
    std::atomic<bool> chainSupported_ { true };
    std::atomic<bool> chainMono_ { false };
    // Preallocated stereo scratch for the mono up-mix (audio thread only).
    juce::AudioBuffer<float> chainScratch_;
    // Mono fold-down uses TAKE-LEFT (never sums — summing can cancel a widener
    // or mid-side plugin to a hollow track). monoFoldDiverges_ is the runtime
    // gate (audio thread): the chain output L and R diverge on the duplicated-
    // mono input, so real stereo work is being dropped. monoStereoOnlyNames_ is
    // the culprit list (message thread, recomputed on every chain change).
    std::atomic<bool> monoFoldDiverges_ { false };
    float             monoDivergeAvg_ = 0.0f;   // audio-thread leaky average
    juce::String      monoStereoOnlyNames_;     // message thread
    void updateMonoStereoOnlyNames();

    void clearChainInternal();              // closes editors first via callback
    void updateChainLatency();
    // Rebuild chainModel from the live host slots after structural edits
    // (Phase 1c): names/bypass/hostIdx from the rack; settings text carried
    // over by first name match from the previous model. Missing (unloadable)
    // model entries do not survive an edit resync — the rack is truth.
    void resyncChainModelFromHost();
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
    // Detected key (KEY_DETECTOR_SPEC.md §9): the Link is where key detection
    // matters most — a Link on the instrumental/mix bus knows the key of the
    // MUSIC, which a main plugin building a vocal chain cannot hear. Fed on
    // the audio thread beside the meter tap (Active only), analysed on the
    // worker thread in continuous mode, published in the frame's key group.
    // Declared engine-then-worker so the worker (which references the engine)
    // is destroyed FIRST.
    echojay::KeyEngine keyEngine_;
    EedKeyWorker       keyWorker_ { keyEngine_ };

    // ---- Passive-detection scheduler (KEY_PRECONDITION_SPEC.md §5.1) -----
    // The engine used to run in continuous mode (a fresh pass every ~2 s of
    // audio, forever). Passive detection replaces that with a DUTY CYCLE:
    // one committed 8 s pass roughly every 30 s, armed only while the
    // transport is rolling AND the channel is above a signal floor, skipped
    // entirely on silence. The key of a track does not change four times a
    // second; this keeps the added CPU invisible next to the metering the
    // Link already pays for. Re-detection outside the cycle happens on real
    // invalidation events only (§5.4): a transport jump to a different
    // section, a long stop-gap, or a remote RE-ANALYSE command.
    static constexpr float    kKeyPassWindowS     = 8.0f;
    static constexpr uint32_t kKeyPassIntervalMs  = 30000;
    // Momentary-LUFS floor for "signal present". Well below programme level
    // but above bleed/noise: a pass armed on -60 LUFS room tone would spend
    // its window measuring the noise floor's key.
    static constexpr float    kKeySignalFloorLufs = -55.0f;
    // A transport position landing this far from where the last block left
    // off is a section jump (a loop wrap of a few bars stays under it).
    static constexpr double   kKeyJumpSeconds     = 30.0;
    // A stop this long, then play again, counts as "came back later" —
    // possibly a different section — so the next pass is due immediately.
    static constexpr uint32_t kKeyLongGapMs       = 120000;

    // Audio thread -> scheduler: transport state read from the playhead.
    std::atomic<bool>   transportPlaying_ { false };
    std::atomic<double> transportTimeS_   { 0.0 };
    std::atomic<bool>   keySectionJump_   { false };
    // Scheduler state (message thread only).
    uint32_t lastKeyPassArmMs_ = 0;
    uint32_t lastPlayingMs_    = 0;
    bool     keyWasPlaying_    = false;
    int      keyStallTicks_    = 0;
    void schedulePassiveKeyPass();

    // Remote RE-ANALYSE: key-cmd-<instanceId>.json {v:1, seq}, acked as
    // key-ack-<instanceId>.json {v:1, seq, status}. Arms a committed pass on
    // THIS engine (the passive reading) — the Meters tab's RE-ANALYSE button
    // for a Link-sourced key. Additive protocol: old Links never poll it,
    // old senders never write it.
    int  lastAppliedKeySeq_ = 0;
    void pollKeyCommand();
    // Audio liveness: processBlock bumps this; the publisher marks frames
    // audioStale when it stops advancing for ~1s (Logic idles silent
    // channels — the engine freezes but heartbeats/timers keep running)
    std::atomic<uint32_t> audioBlockCounter_ { 0 };
    uint32_t lastSeenBlockCount_ = 0;
    uint32_t lastBlockAdvanceMs_ = 0;
    bool     audioWasStale_ = false;   // transition logging
    void publishMeterFrame();

    // Per-INSTANCE identity: generated at construction, serialised with
    // state (survives reopen), regenerated on registry collision (track
    // duplication clones state incl. the uid). Commands/acks/registry key
    // on THIS — the display name is a label, never an address (unnamed or
    // same-named Links collided and toggles applied to all of them).
    juce::String instanceUid_;
    // Host track name stash (see the Phase N block above). appliedHostName_
    // is message-thread-only change detection for the timer's apply pass.
    mutable juce::CriticalSection hostNameLock_;
    juce::String hostTrackName_;               // guarded by hostNameLock_
    std::atomic<bool> hostNameDirty_ { false };
    juce::String appliedHostName_;
    // Rack sidecar (Phase R): last ChainHost revision written to
    // rack-<uid>.json. -1 so the first tick publishes even an empty rack
    // ("known empty" is a different fact from "rack unknown").
    int lastPublishedRackRev_ = -1;
    // Last hosted-parameter epoch written. Separate from the revision because
    // the two publish on different terms: structure at once, knobs after they
    // settle. -1 for the same reason as above.
    int lastPublishedEpoch_ = -1;
    // SETTLE TIME for an epoch-only republish. Deliberately NOT ChainHost's
    // kStateDebounceMs (2000ms): that exists to avoid serialising a whole
    // plugin's state blob mid-drag, an expensive operation this one is not.
    // Recomputing 64 analytic magnitudes and rewriting a small JSON file is
    // cheap, so it waits only long enough to coalesce a gesture.
    //
    // THE HONEST END-TO-END NUMBER: publishRackSidecar is polled every 3rd
    // 30Hz tick, so the settle is TESTED at 100ms granularity and fires 200
    // to 300ms after the last knob event. The main plugin then re-reads
    // sidecars at ~1Hz, so a knob turn reaches a drawn curve up to ~1.3s
    // later. That is a thumbnail refresh rate, not a live meter, and the
    // drawing side is written to look like one (see the strip's EQ paint).
    static constexpr double kRackSettleMs = 200.0;
    // STALENESS BOUND, the companion to the settle. Sustained automation
    // moves a parameter every block, so the settle test alone would never
    // fire and the curve would freeze precisely while it was moving most.
    // Whichever comes first wins, so a gesture coalesces and a ramp still
    // gets roughly one publish a second, matching the reader's own cadence.
    static constexpr double kRackMaxStaleMs = 1000.0;
    double lastRackPublishMs_ = 0.0;
    // THE CURVE IS POLLED because the built-in devices notify nothing (see
    // publishRackSidecar). Two copies, not one, and they are different facts:
    // `lastComputed` is what the engine said on the PREVIOUS tick and dates
    // the settle timer; `lastPublished` is what is actually in the file and
    // decides whether a write is owed. Collapsing them would make a slow drag
    // read as "still moving" forever, because the published value is meant to
    // lag.
    std::vector<int16_t> lastComputedCurve_;
    std::vector<int16_t> lastPublishedCurve_;
    // Pre-gain is EchoJay's own gain, not a hosted parameter, so it emits no
    // change notification (same reason the EQ curve is POLLED). Tracked here
    // so publishRackSidecar republishes when it moves, from ANY source (the
    // Link's own knob or a remote command), keeping the ONE mirror current.
    float lastPublishedPreGainDb_       = 0.0f;
    bool  lastPublishedPreGainUserSet_  = false;
    bool  lastPublishedPreGainInputKnown_ = false;
    double lastCurveChangeMs_ = 0.0;
    void publishRackSidecar();
    // ---- Edit lease (stage 1 remote editing) ------------------------------
    // The pure gate decides; this class acts. leaseActive_ is the ONE field
    // the audio thread reads (it forces ring production); everything else is
    // message-thread state. leasePriorBypass_ is what the restore restores:
    // the bypass the USER had before the lease, not blanket false.
    void pollEditLease();

public:
    // ---- Rack lock (21 Aug 2026, RACK_BORROW_REQUIREMENTS §4) -------------
    // UI-ONLY ownership: while a main's editor shows this rack, the six local
    // mutation entry points refuse and the editor greys. NEVER audio: no
    // bypass, no Active, nothing in the graph. The ctrl-cmd path is NOT
    // guarded here — that is the lock OWNER'S write path.
    // Empty = unlocked; else the owning main's display name for the overlay.
    juce::String rackLockOwner() const { return rackLockOwner_; }
    // true = refused (and said so in the log — a guard that can silently do
    // nothing must assert that it did something).
    bool rackLockGuard(const char* op);
    // Stamp a LOCAL rack edit for the recency rule. Local only, never remote
    // ops: deriving this from chainRevision would also stamp the main's own
    // edits and make it wait out itself after releasing.
    void stampLocalRackEdit()
    { lastLocalRackEditMs_ = (double) juce::Time::currentTimeMillis(); }

private:
    void pollRackLock();
    juce::String rackLockOwner_;             // message thread only
    double       lastLocalRackEditMs_ = 0.0; // message thread only

    LinkShm::LeaseGate leaseGate_;
    std::atomic<bool>  leaseActive_ { false };
    int                leaseSlot0_       = -1;
    bool               leasePriorBypass_ = false;
    // Whole-rack lease (step 2): the saved bypass of EVERY slot, restored by
    // the same Expire/Release arm the slot lease uses. Message thread only.
    bool               rackLeaseActive_ = false;
    std::vector<bool>  rackLeasePrior_;
    bool               planJournalChecked_ = false;   // once per process
public:
    /** Phase 3: is a structure-plan journal active for this Link? Drives
        the lock overlay's restructuring line — the statement that the
        shape flicker underneath is deliberate, not corruption. */
    bool structPlanJournalPresent() const
    {
        return resolvedDir.isNotEmpty() && instanceUid_.isNotEmpty()
            && juce::File(LinkShm::StructureEdit::journalPath(resolvedDir,
                                                              instanceUid_))
                   .existsAsFile();
    }
private:

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
