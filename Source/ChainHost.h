#pragma once
#include <JuceHeader.h>
#include "PluginScanner.h"
#include "EedDeviceRegistry.h"
#include "EchoJayLevelTally.h"
#include <atomic>
#include <map>
#include <set>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

// Manages plugin discovery and hosting via AudioProcessorGraph.
// Owned by EchoJayProcessor.
//
// Discovery — list-then-load-on-demand, no bulk instantiation:
//   startScan() reads names/metadata ONLY.
//   AU: CoreAudio AudioComponentFindNext() — pure registry query, no dylib
//   VST3: filesystem walk, name from bundle filename, no loading
//   Results deduplicated: AU preferred over VST3 of same name.
//
// Hosting — ordered chain of AU/VST3 plugins in series:
//   loadPluginAsync() appends a new slot to the end.
//   Bypassed slots are skipped in routing (passthrough) but remain in list.
//   rebuildGraph() runs on the message thread; JUCE's internal graph lock
//   makes message-thread modifications and audio-thread processBlock safe.
class ChainHost : private juce::AudioProcessorListener
{
public:
    // Lightweight slot description safe to copy to the UI thread
    struct SlotInfo {
        juce::String name;
        bool bypassed;
        juce::String settings;  // suggested dial-in guidance from AI (display only)
        juce::String format;    // "AudioUnit" / "VST3" — popout-only is per-format
        float wet = 1.0f;       // per-slot wet/dry (0..1, 1 = fully wet)
    };

    ChainHost();
    ~ChainHost() override;

    // ---- Audio thread hooks -----------------------------------------------
    void prepare(double sampleRate, int blockSize);
    void release();
    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    // ---- List refresh (message thread) -----------------------------------
    void startScan();
    // RECORD, no fix (13 Aug 2026): cancelScan has NO CALLERS, so the
    // cancel early-return inside doRefresh is unreachable and misleads
    // anyone tracing how a scan can end. A scan ends by completing.
    void cancelScan();
    bool  isScanning()      const noexcept { return scanning_.load(); }
    float getScanProgress() const noexcept { return scanProgress_.load(); }
    juce::String getScanStatus() const;
    // When the entries list was last actually SCANNED (epoch ms; 0 = the
    // cache predates the stamp). Written into chain_entries.xml at scan
    // completion, read back at cache load, shown in the Chain tab. Exists
    // because a July cache served five weeks of sessions while the UI
    // confidently reported a plugin count with no date on it.
    juce::int64 getEntriesScannedAtMs() const noexcept { return entriesScannedAtMs_; }

    // ---- Plugin list (message thread) ------------------------------------
    int getNumPlugins() const;
    // collapseTwins: with an EMPTY formatFilter the list is collapsed
    // AU-preferring (a VST3 row whose exact name has an AU row is hidden).
    // false keeps both builds, for a picker whose rows are labelled by
    // format; the VST3-in-AU-host experiment needs it so "Pro-Q 3 [VST3]"
    // is rackable beside "Pro-Q 3 [AU]". The model feed is name-keyed and
    // never gets both, by construction.
    juce::Array<juce::PluginDescription> getFilteredPlugins(
        const juce::String& filter,
        const juce::String& formatFilter = {},
        bool collapseTwins = true) const;

    // Why a scanned row is withheld from the browser and the feed (15 Aug
    // 2026). ONE function decides; getFilteredPlugins, resolveByName and
    // buildRecommendable all call it, so the predicate and the reason
    // cannot disagree. Settings draws the reason; nothing here renders.
    //   CrashBlacklisted          on chain_blacklist.txt (withheld)
    //   ArchitectureIncompatible  VST3 with no slice for this process (withheld)
    //   Unreadable                VST3 whose binary could not be judged. KEPT:
    //                             it is in the enum so a diagnostic can tell
    //                             it from Loadable, never so it can be filtered.
    //   None                      kept
    // The format filter is not a withhold (a per-host view) and is applied
    // separately at each site. Cheap per row: the arch memo already exists.
    // SettingsTooLarge (17 Aug 2026): the plugin's settings at their defaults
    // are over the per-plugin cap a SESSION can save, so a chain holding it
    // could not be saved with the project; its own reason and its own file
    // (chain_state_oversize.txt), never the crash list. Measured in the
    // fingerprint pass and at first rack (completeLoad); the file is the
    // authority and deleting a line offers the plugin again.
    enum class WithholdReason { None, CrashBlacklisted, ArchitectureIncompatible, Unreadable, SettingsTooLarge };
    static bool isWithheld(WithholdReason r) noexcept
    {
        return r == WithholdReason::CrashBlacklisted
            || r == WithholdReason::ArchitectureIncompatible
            || r == WithholdReason::SettingsTooLarge;
    }
    // Bytes recorded for a SettingsTooLarge path (0 when not recorded)
    int oversizeStateBytes(const juce::String& path) const;
    static juce::File getStateOversizeFile();
    WithholdReason withholdReason(const juce::PluginDescription& d) const;   // takes pluginsMutex_
    // User-facing clause completing "\"<name>\" ...", e.g. "is installed but
    // its VST3 has no arm64 build, so it cannot run in this host". Empty for
    // None and Unreadable (nothing to explain: the row is offered).
    static juce::String withholdReasonText(WithholdReason r);

    // ---- Settings ↔ ChainHost resolver (message thread) -----------------
    // A "recommendable" plugin is BOTH enabled in the Settings checklist AND
    // present in ChainHost's loadable entries_ list (same format filter applies).
    //
    // Call buildRecommendable() after a scan completes or after Settings are
    // saved, passing the current enabled plugin list and the active format filter.
    // The result is cached internally and returned by the two accessors below.
    struct RecommendableEntry {
        juce::String            displayName;
        juce::PluginDescription desc;
    };
    void buildRecommendable(const std::vector<ScannedPlugin>& enabledPlugins,
                            const juce::String& formatFilter);

    // Display names of resolved entries (for AI prompt injection).
    juce::StringArray getRecommendableNames() const;

    // name -> fingerprint for the chat body's mapFps field (per-fp exact
    // controls exposure, 9 Aug 2026). Rack slots first - a loaded slot's fp
    // IS the binary the user holds - then recommendable entries resolved
    // through the persistent identity index (scan-path fingerprint pass).
    // A name carried twice with DISAGREEING fps is omitted entirely: the
    // server's sibling merge is the honest serve for an ambiguous name. A
    // name with no known fp is simply absent, same fallback. Returns "{}"
    // when nothing is known.
    // maxEntries RAISED FROM 64 (11 Aug 2026). The rack is never capped (a
    // loaded slot's fp is the exact binary and always goes first); this bounds
    // the RECOMMENDABLE set, and on a build turn the rack is empty so the
    // whole budget went to the first 64 of a ~740-plugin scan. A live turn
    // showed picked=4 fromFp=0 noFp=4: not one chosen plugin had a
    // fingerprint, which left every fp-keyed path on the server answering
    // "unknown" and falling back to the sibling-merged view.
    //
    // The server parses up to MAP_FPS_MAX (2000) and both must move together:
    // raising one alone changes nothing, since the smaller cap still binds.
    juce::String buildMapFpsJson(int maxEntries = 2000) const;

    // ---- Apply-time honesty (26 Jul 2026) ----
    // Per-slot auto-dial outcome. The result bubble may only relay the
    // model's "result" line when every slot that carried structuredSettings
    // actually APPLIED in full; everything else composes factual wording
    // naming the hand-dial slots/controls.
    //   none        — no structuredSettings for this slot (nothing expected)
    //   pending     — map fetch in flight, outcome unknown yet
    //   applied     — every requested semantic written
    //   partial     — some written, some not (manual lists the misses)
    //   noMap       — no local map for this fingerprint, nothing written
    //   unusableMap — map exists but none of the REQUESTED semantics were
    //                 writable, nothing written
    enum class DialStatus { none, pending, applied, partial, noMap, unusableMap };
    struct SlotDialInfo {
        juce::String      name;
        juce::String      fp;          // fingerprint (for event logging)
        DialStatus        status = DialStatus::none;
        juce::StringArray manual;      // human labels of unwritten controls
        juce::StringArray readbackMiss; // subset of manual: wrote wrong, reverted
        juce::StringArray unconfirmed; // written and KEPT on norm proof; display
                                       // read was stale (bridged AU, report-only)
        int               appliedCount = 0;
        juce::String      staleIndexedFp; // stale-map ladder: superseded fp,
                                          // "" = load matched the index
        juce::StringArray outOfRange;     // subset of manual: refused by range validation
    };
    std::vector<SlotDialInfo> getDialInfos() const;
    // One line per slot, at the END of a build, saying what actually dialled.
    // The per-call lines cannot answer "nothing dials" because each is a
    // snapshot mid-sequence and the benign ones outnumber the real ones; this
    // is the terminal state, when every slot has had its chance. Call it when
    // a chain build finishes loading.
    void logDialSummary (const juce::String& reason) const;
    // Settle, then report -- and it lives HERE, not in an editor, because
    // ChainHost.cpp is compiled into BOTH binaries while PluginEditor.cpp is
    // not. Every dial instrument used to hang off the main plugin's editor, so
    // a Link-side build dialled (or failed to) in total silence. A report that
    // exists on one of two paths is not a report.
    void reportDialWhenSettled (const juce::String& reason, int attemptsLeft = 8);
    // True when no slot is DialStatus::pending (bubble may compose).
    bool dialStateSettled() const;
    // Recommendable display names whose local map passes the dial-signals
    // threshold (>=2 usable CORE semantics). Used by the dark 2.1 markers
    // and 2.4 dialFlags; shares echojay::mapIsDialableForSignals.
    juce::StringArray getDialableRecommendableNames() const;

    // Count stats from the last buildRecommendable() call.
    int getRecommendableCount()   const noexcept { return (int)recommendable_.size(); }
    int getEnabledInputCount()    const noexcept { return recommendableEnabledIn_; }
    int getUnmatchedCount()       const noexcept { return recommendableEnabledIn_ - (int)recommendable_.size(); }

    // True once buildRecommendable() has run against real inputs (non-empty
    // entries AND ≥1 enabled scanner plugin). Distinguishes resolved (even
    // resolved-with-zero-matches) from scanning/unresolved, where the inputs
    // weren't ready yet and callers should keep retrying. Message thread only.
    bool hasResolvedRecommendable() const noexcept { return hasResolved_; }
    // Tick-state freshness (13 Aug 2026): the editor calls this when the
    // disabled-uids authority changes (maybeReloadEnabledState), so the
    // next timer tick rebuilds the feed against fresh ticks instead of
    // waiting for a restart.
    void invalidateRecommendable() noexcept { hasResolved_ = false; }

    // Async-load the first recommendable entry whose displayName matches `name`
    // (case-insensitive). Callback: empty string on success, error message on fail.
    void loadByRecommendedName(const juce::String& name,
                               std::function<void(const juce::String&)> callback);

    // ---- Chain slot management (message thread) --------------------------
    // Async-append: loads the plugin and adds it to the end of the chain.
    // callback(error) — empty on success.
    void loadPluginAsync(const juce::PluginDescription& desc,
                         std::function<void(const juce::String& error)> callback);

    int                      getNumSlots()    const noexcept;
    std::vector<SlotInfo>    getAllSlotInfos() const;
    SlotInfo                 getSlotInfo(int i) const;

    void removeSlot(int i);
    void moveSlot(int i, int direction);    // direction: -1 = left, +1 = right
    void setSlotBypassed(int i, bool bypassed);
    void setSlotSettings(int i, const juce::String& settings);  // store AI guidance text

    // ---- Structural edit operations (CHAIN_AI_BUILD_SPEC Phase 1c) --------
    // Ops arrive from the <<<ECHOJAY_CHAIN_EDIT>>> block; every index refers
    // to the rack numbering the model saw (the 0-based [CURRENT CHAIN]
    // injection), never renumbered mid-sequence — the sequencer translates
    // through an original->current map as earlier ops shift the rack.
    struct ChainEditOp {
        juce::String op;        // add | remove | replace | move | bypass | set | set_wet
        int  slot  = -1;        // original index (remove/replace/move/bypass/set/set_wet)
        int  to    = -1;        // move target (original numbering)
        int  after = -2;        // add: insert after this original slot; -1 = first
        bool on    = false;     // bypass state
        // Slot wet/dry from the model (16 Aug 2026): "wet_pct" 0..100 on a
        // set_wet op (required there) or riding an add/replace (applied once
        // the slot has loaded). -1 = absent = leave the knob alone.
        float wetPct = -1.0f;
        juce::String name;      // add/replace: name from AVAILABLE PLUGINS
        juce::String settings;  // prose settings for the slot tile (display)
        // Server-decided no-such-control verdict riding the op (9 Aug
        // 2026): term the user asked for + provenance tier (deferred /
        // unmapped / complete). The card composes the REASON a suggestion
        // is hand-dial-only from this, never from the model's prose.
        juce::String noSuchTerm, noSuchTier;
        // add/replace: the machine-readable settings_structured that rides the
        // op, applied once the new slot has loaded. Same payload and same
        // consumer as the build path, so an EQ added by an EDIT turn is dialled
        // exactly like one added by a chain build instead of arriving flat.
        //
        // MERGE NOTE (feat/v2-with-devices): both branches independently grew
        // this field — the dashboard line called it `settingsStructured`, the
        // device line `structuredSettings`. One field, one name: it keeps the
        // device line's spelling because that is what Slot::structuredSettings
        // is already called, and the two are the same payload end to end.
        juce::var    structuredSettings;
    };
    // Parse edit-block JSON. Returns empty on malformed payloads. Static so
    // preview cards can humanize ops without touching a host.
    static std::vector<ChainEditOp> parseChainEditOps(const juce::String& editJson,
                                                      juce::StringArray* baseSlotsOut = nullptr,
                                                      juce::String* explanationOut = nullptr);
    // One plain-language line per op for the preview card ("+ add X after
    // slot 1 (name)"). baseSlots supplies the slot names.
    static juce::String describeEditOp(const ChainEditOp& op,
                                       const juce::StringArray& baseSlots);

    // Apply ops serially on the message thread (shared by main plugin and
    // Link, like the wet/dry work). CONTRACT: the caller has closed every
    // hosted editor at least 80ms before calling (the AMEK editor-close
    // discipline); removeSlot itself never destroys instances (graveyard),
    // so mid-sequence removals need no further delays.
    //
    // Safety model:
    //  - expectedRevision >= 0 must equal the live chainRevision (pass -1 to
    //    skip: cards restored from a previous session, where the in-memory
    //    counter reset). baseSlots is ALWAYS verified against the live rack.
    //  - every op is pre-validated against a simulated rack BEFORE any
    //    mutation; a static problem aborts the whole edit untouched.
    //  - within an op, fallible async work (plugin load) happens BEFORE any
    //    destructive step, so a failed load leaves the rack unchanged by
    //    that op. On a runtime failure: stop, keep the applied prefix, skip
    //    the rest — never roll back, never half-apply an op.
    // onDone(results (one line per op, or the abort reason), appliedCount,
    //        abortedBeforeStart).
    void applyChainEdits(std::vector<ChainEditOp> ops,
                         int expectedRevision,
                         const juce::StringArray& baseSlots,
                         std::function<void(const juce::StringArray& results,
                                            int appliedCount,
                                            bool abortedBeforeStart)> onDone,
                         std::function<void(const juce::String& label)> onProgress = {});
                         // onProgress (Phase 1d): fired on the message thread
                         // as each op STARTS, with a real present-tense label
                         // ("Loading SPL De-Esser...") — feeds the staged
                         // working-state shimmer. Never fires a claim that
                         // could desync: labels describe the op being
                         // attempted, results report what happened.

    // ---- Chain revision (CHAIN_AI_BUILD_SPEC Phase 1a staleness guard) ----
    // Monotonic counter bumped on EVERY chain mutation: add (completeLoad),
    // remove, move, bypass, per-slot wet, master wet. An AI edit proposal
    // snapshots this at propose time; the apply (Phase 1c) aborts when the
    // live value differs — the rack changed between propose and confirm.
    // Atomic so any thread may read; mutations happen on the message thread.
    int getChainRevision() const noexcept { return chainRevision_.load(std::memory_order_relaxed); }

    // ---- Hosted-parameter epoch (the curve's publish trigger) --------------
    // chainRevision covers STRUCTURE and nothing else: add, remove, move,
    // bypass, wet. Turning a knob INSIDE a hosted plugin bumps none of them,
    // so anything gated on the revision alone freezes at whatever the rack
    // last looked like when a plugin was added. This counter covers exactly
    // that gap: it is bumped from audioProcessorParameterChanged, which the
    // hosted plugins already call.
    //
    // REACHABLE FROM THE AUDIO THREAD during automation, so the write side is
    // two relaxed atomic stores and nothing else. Readers poll it; it shares
    // a numbering space with nothing, and like the revision it is only ever
    // compared with a value read from the SAME ChainHost.
    int getHostedChangeEpoch() const noexcept
        { return hostedEpoch_.load(std::memory_order_relaxed); }

    /** Milliseconds (getMillisecondCounterHiRes clock) of the last hosted
        change of any kind. Lets a poller wait for a gesture to SETTLE rather
        than republishing on every event of a knob drag. */
    double lastHostedChangeMs() const noexcept
        { return lastChangeMs_.load(std::memory_order_relaxed); }

    /** Fill out[0..n) with the FIRST built-in EQ's magnitude response,
        quantised to integer deci-dB on the LinkShm::eqCurveFreqs grid.
        n must be LinkShm::kEqCurvePoints.

        Returns FALSE when the rack has no built-in EQ, when the slot cannot
        answer, or when the engine returns a non-finite sample. On false the
        caller must publish NOTHING: a fabricated flat curve would claim the
        EQ is doing nothing, which is a different statement from "no reading".

        Message thread. getMagnitudeResponse is analytic over the published
        targets and allocates nothing, but it is not an audio-thread call. */
    bool getBuiltinEqCurveDeciDb(int16_t* out, int n);

    // ---- Wet/dry (house pattern: internal smoothed state, NOT host params) --
    // Per-slot: blended inside the graph by a SlotWetBlend node whose dry leg
    // is latency-aligned by the graph's render sequence. Master: blended in
    // process() against a latency-delayed dry copy of the chain input.
    // Setters are message-thread; the audio thread reads atomics. Knob drags
    // are rebuild-free — values flow through shared atomics, never rewiring.
    void  setMasterWet(float wet01);
    float getMasterWet() const noexcept { return masterWet_.load(std::memory_order_relaxed); }
    void  setSlotWet(int i, float wet01);
    float getSlotWet(int i) const;

    // ---- Running level (LevelTally, 17 Aug 2026) --------------------------
    // The chain input (pre-graph, even with an empty rack, so a build on an
    // empty rack still knows the level), the chain output (post master wet,
    // pre bus trim: the chain's own output), and each ACTIVE slot's input
    // and output, measured inside its SlotWetBlend on both legs. A bypassed
    // slot has no blend node and is not measured. Snapshots are honest by
    // construction: known == false and NaN numbers until the floor of gated
    // audio has been heard. Message thread.
    struct SlotLevels { echojay::LevelTally::Snapshot in, out; bool measured = false; };
    echojay::LevelTally::Snapshot getChainInLevels() const  { return chainInTally_.snapshot(); }
    echojay::LevelTally::Snapshot getChainOutLevels() const { return chainOutTally_.snapshot(); }
    SlotLevels getSlotLevels(int i) const;
    void resetAllLevels();   // source change, manual reset

    // ---- Pre-chain gain (headroom + operating level, 18 Aug 2026) ------
    // A gain applied BEFORE the first slot. Three jobs (Sean): headroom into
    // the rack (a trim EchoJay never had), a known operating level so
    // analogue-modelled plugins behave predictably, and an output that lands
    // near the input because the chain was DESIGNED at that level, not
    // compensated afterward. chainInTally_ still measures the RAW input
    // (before this gain); slot 1's own input tally then reads the post-trim
    // OPERATING level, which the model chooses settings against.
    //
    // At build EchoJay sets it from the measured input to reach the target,
    // UNLESS the user set it by hand (preGainUserSet_), which a build never
    // overwrites. known == false: not set, no guess, the reason is visible.
    // The value is EchoJay's own gain, never a plugin parameter, and is a
    // user-owned control (range below). Persisted with the SESSION; a shared
    // chain does NOT carry it (it is source-specific and recomputed locally).
    static constexpr float kPreGainTargetLufs = -18.0f;  // analogue reference (0 VU) in the level layer's unit
    static constexpr float kPreGainMinDb = -24.0f, kPreGainMaxDb = 24.0f;
    enum class PreGainState { Off, Auto, UserSet, NoLevel };
    struct PreGainReadout { float db = 0.0f; PreGainState state = PreGainState::Off; bool userSet = false; juce::String text; };
    float getPreGainDb() const noexcept { return preGainDb_.load(std::memory_order_relaxed); }
    bool  isPreGainUserSet() const noexcept { return preGainUserSet_; }
    // User (knob/menu): userSet true stamps the hand-set flag so a build
    // leaves it. resetPreGainToAuto clears the flag so the next build sets it.
    void  setPreGainDb(float db, bool userSet);
    void  resetPreGainToAuto();
    // At BUILD only (loadChainFromJson). Never on an edit.
    void  computePreGainAtBuild();
    PreGainReadout getPreGain() const;
    // The operating level the settings are chosen against = raw input + the
    // applied pre-gain. NaN when the input level is not known.
    float getOperatingLevelLufs() const;
    // ---- Persistence of the running level ("chainLevels", beside the frozen
    // chainSlotsXml). The tally is a claim about the SOURCE, so it carries the
    // host's track name and is discarded on restore when the names differ:
    // a plugin copied to another track starts empty rather than inheriting
    // a confidently wrong level. Where NEITHER side names the track the
    // tally is discarded too (nothing can tie it to a source); it restarts
    // after a few seconds of playing. Slot tallies are keyed by saved slot number
    // and land as each slot restores (restoreNextSlot); chain in/out land at
    // once. Message thread.
    juce::var getLevelsStateVar(const juce::String& trackName) const;
    void      setPendingLevelsState(const juce::var& v, const juce::String& currentTrackName);
    // The host's name for this track as last reported (empty = unknown).
    // A CHANGE of name, or a first name that differs from the one a restored
    // tally was measured on (the host reported the name after the state
    // arrived), resets the tallies: same guard, both orderings.
    void      setHostTrackName(const juce::String& name);
    juce::String getHostTrackName() const { return hostTrackName_; }

    // EchoJay auto-parameter-mapping: dial a slot's hosted plugin from
    // structured settings plus the plugin's map.
    struct ApplyReport { juce::String semantic; bool applied; float normalized; juce::String note;
                         juce::String landedText; bool displayVerified = false; bool readbackMismatch = false;
                         bool staleDisplayKept = false; juce::var requestedValue;
                         bool outOfRange = false; };
    std::vector<ApplyReport> applyStructuredSettings (int slotIndex,
                                                      const juce::var& structuredSettings,
                                                      const juce::var& map);

    // ---- Auto-apply pipeline (the ONE apply path) ------------------------
    // A chain reply's per-slot settings_structured object is handed to the
    // slot here after load; the apply fires the moment the plugin's map is
    // available (immediately when cached, else on fetch completion). No map
    // or no structured settings -> silent skip, prose display stays as-is.
    void setSlotStructuredSettings (int i, const juce::var& structured);

    // Store maps fetched from GET /api/params/maps ({fp: map|null} object),
    // persist them, and apply any slots that were waiting on them.
    void storeParamMaps (const juce::var& mapsObj);

    // Batch-prefetch (via onNeedParamMaps) maps for every fingerprint the
    // persistent identity index knows but has no cached map for, <=500 per
    // request. Called after scans so Build needs no round trip. Fps already
    // requested this session are not re-requested.
    void requestMapPrefetch();

    // Fingerprint pass (scan path): computes the server fingerprint
    // sha256(format|uidHex|version|param_count) for every scanned effect
    // entry not yet in the persistent identity index. param_count only
    // exists on a constructed instance, so each plugin is instantiated ONCE,
    // sequentially, on the message thread (JUCE requires it), fingerprinted
    // and released. Runs automatically when a scan completes. Skips: thin
    // VST3 entries (empty version would hash a WRONG basis; their first real
    // load fingerprints them in completeLoad), instruments (never chain
    // slots), and identities whose previous attempt failed or crashed the
    // host (marker persisted BEFORE each load, cleared on success).
    void startFingerprintPass();
    bool isFingerprintPassRunning() const { return fpPassRunning_.load(); }

    // Networking is the editor's job (EchoJayAPI lives there): ChainHost
    // asks for fps through this and receives results via storeParamMaps.
    std::function<void(const juce::StringArray& fps)> onNeedParamMaps;

    // Fired after an async auto-apply mutates a slot's settings display so
    // the rack UI can refresh. Always called on the message thread.
    std::function<void()> onSlotSettingsChanged;

    static juce::File getParamMapsCacheFile();

    // Shared dev gate. Logic hosts AUs in the sandboxed AUHostingService,
    // where userApplicationDataDirectory resolves into the service's
    // container, NOT the real ~/Library — so the container-relative dev_mode
    // file is invisible in-DAW. The fixed absolute path works sandboxed; the
    // container-relative check stays as the fallback for standalone/unboxed.
    static bool devModeActive()
    {
        return juce::File("/Users/SeanD/.echojay_dev").existsAsFile()
            || juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("EchoJay").getChildFile("dev_mode").existsAsFile();
    }

    // EXPERIMENT (17 Aug 2026): offer VST3 rows inside an AU host. Off by
    // default everywhere, dev mode included; on only when BOTH dev_mode and
    // ~/Library/EchoJay/vst3_in_au_host exist. It widens what the picker and
    // the model feed OFFER (the editor's chainFormatFilter_ goes empty under
    // AU); the load path has never checked format, so a session or shared
    // chain holding a VST3 restores here with the flag off too, and
    // completeLoad says so in the rack. Read at editor construction and at
    // processor construction (the one-time log).
    static bool vst3InAuHostExperiment()
    {
        return devModeActive()
            && juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("EchoJay").getChildFile("vst3_in_au_host").existsAsFile();
    }

    // The wrapper this ChainHost lives in ("AudioUnit", "VST3", or empty for
    // standalone / Link, which does not set it). Set by the processor at
    // construction; completeLoad uses it to note, in the rack, a slot whose
    // build is not the host's own format.
    void setHostPluginFormat(const juce::String& f) { hostPluginFormat_ = f; }

    juce::AudioProcessorEditor* createEditorForSlot(int i);

    // ---- Shared name resolution (both hosts MUST behave identically) ----
    // The AI feeds names in two formats: plain ("CREAM2PRE") from the chain
    // injection, and "Name (Manufacturer)" from the general plugin feed —
    // models copy either into chain blocks. Loose matching handles both,
    // plus case/whitespace/punctuation/version drift via normalizeName.
    static juce::String stripParenthetical(const juce::String& raw);
    static bool namesMatchLoose(const juce::String& incoming,
                                const juce::String& entryName);

    // Resolve an incoming chain-entry name against the loadable entries.
    // matchLogOut (optional) receives the match path taken, or the closest
    // candidates on failure — for build-time diagnostics.
    //
    // Honest miss (15 Aug 2026): a name that matches a row this host
    // WITHHOLDS (isWithheld) still returns empty, so nothing resolves that
    // cannot load, but the miss says which it was: matchLogOut reads
    // "WITHHELD (<reason>) -> ..." and *withheldOut receives the reason.
    // A genuine miss leaves *withheldOut at None. Callers turn that into
    // "cannot run in this host" instead of "not found", which is the one
    // case where the user can act on the truth.
    juce::PluginDescription resolveByName(const juce::String& rawName,
                                          const juce::String& formatFilter,
                                          juce::String* matchLogOut = nullptr,
                                          WithholdReason* withheldOut = nullptr) const;

    // ---- Built-in devices (EchoJay-owned nodes hosted as ordinary slots) ---
    // The chain otherwise only hosts what formatManager_ can instantiate from
    // a SCANNED description. EchoJay's own EQ has no scanned description, so
    // it travels as a synthetic juce::PluginDescription whose format name is
    // kBuiltinFormat. loadPluginAsync recognises that and builds the node
    // directly instead of calling the format manager.
    //
    // Putting the branch there rather than at each caller is deliberate: the
    // add-plugin menu, AI chain-edit ops, loadByRecommendedName and session
    // restore ALL already funnel through loadPluginAsync, so one branch makes
    // every one of those paths work — including restore, which cannot go
    // through the format manager for a built-in by definition.
    static constexpr const char* kBuiltinFormat = kEchoJayBuiltinFormat;

    // Every built-in device the chain can host, by exact name — GENERATED from
    // BuiltinDeviceRegistry, which each device adds itself to from its own .cpp
    // (BUILTIN_SUITE_PLAN.md §1). The AI feed advertises from here (see
    // EchoJayAPI::buildPluginInjection) and the backend offers a built-in only
    // when its exact name appears in that advertisement, so a new device becomes
    // offerable by existing — no list to edit here, and no version pin on either
    // side.
    static juce::StringArray builtinDeviceNames()
    {
        return BuiltinDeviceRegistry::instance().names();
    }

    // Canonical synthetic description for a named built-in, or an empty
    // description when the name is not one of ours. Stable across machines and
    // sessions: it is what gets written into the saved chain XML and matched on
    // restore.
    static juce::PluginDescription builtinDescriptionFor (const juce::String& rawName);
    static bool isBuiltinDescription (const juce::PluginDescription& d) noexcept;
    static bool isBuiltinName        (const juce::String& rawName);

    // True when slot i holds ANY built-in device (used to route exact apply).
    bool isBuiltinSlot (int i) const;

    // True when slot i holds the built-in EQ specifically. Only for things that
    // are genuinely EQ-shaped (the dev /eqtest command, the editor's EQ panel) —
    // dispatch and hosting must use isBuiltinSlot instead.
    bool isBuiltinEqSlot (int i) const;

    // DEV ONLY. Apply a hand-written eq_bands JSON to a built-in EQ slot,
    // exercising the exact-apply path without the backend (the server
    // contract that emits eq_bands is not deployed yet). Returns a summary,
    // or a human-readable reason it did nothing. Callers gate on
    // devModeActive(); this is inert in a release session because nothing
    // reachable calls it there.
    juce::String devApplyEqJson (int slotIndex, const juce::String& json);

    // Index of the first built-in EQ slot, or -1. Used by the dev command to
    // pick a target when the selected slot is not an EQ. Genuinely EQ-specific
    // (the /eqtest command writes eq_bands), so it stays keyed to the EQ rather
    // than to "any built-in".
    int findFirstBuiltinEqSlot() const;

    // Full-entries cache (chain_entries.xml): written after every scan so
    // the OTHER host resolves against the same list without scanning.
    // maybeReloadEntriesCache() re-loads when another host refreshed it.
    static juce::File getEntriesCacheFile();
    void maybeReloadEntriesCache();

    // ---- Popout-only plugins (both hosts) ----------------------------------
    // Plugins whose editors can only render in a floating window: out-of-
    // process views (WaveShell etc.) attach as an AUv2ContainerView proxy
    // that never negotiates a real size inside a clipped container, but works
    // in its own NSWindow. Once a plugin times out inline it is remembered in
    // ~/Library/EchoJay/popout_only.txt (normalised name + format keyed,
    // mtime-reloaded so both hosts see marks immediately) and future
    // selections go straight to the pop-out with no failed inline attempt.
    // FORMAT-QUALIFIED because the same plugin's VST3 build is in-process
    // and may contain fine when the AU cannot.
    static bool isPopoutOnly(const juce::String& pluginName, const juce::String& format);
    static void markPopoutOnly(const juce::String& pluginName, const juce::String& format);

    // ---- Session load failures (deliberately NOT persisted) ---------------
    // A load failure means "could not authorise RIGHT NOW" (iLok not
    // plugged into this machine, licence server unreachable, transient OS
    // error) — NOT "not owned". Persisting exclusions from that signal
    // would suppress genuinely-owned plugins on a machine whose dongle is
    // elsewhere, so the set lives in memory only: it excludes plugins from
    // the AI feed (buildRecommendable) for the REST OF THIS SESSION, and a
    // plugin reload starts clean. Cleared per-entry by any successful load.
    // Edit/build resolution stays unfiltered (rack plugins always
    // referenceable; a deliberate retry remains possible).
    // See sessionLoadFailed_ below; no public API — internal to ChainHost.

    // ---- Host session identity ---------------------------------------------
    // The user-facing HOST process this plugin instance ultimately belongs
    // to. Inside an out-of-process AU host (AUHostingServiceXPC) this is the
    // DAW itself (Logic Pro), resolved via the responsible-pid API with a
    // parent-walk backstop; for in-process hosts it is simply our own
    // process. startSec/startUsec come from PROC_PIDTBSDINFO so a recycled
    // pid can never false-match. `degraded` means resolution stopped at a
    // helper process: sharing is limited to that process, but stale values
    // from a previous session still cannot be adopted.
    struct HostIdentity
    {
        int          pid       = 0;
        juce::int64  startSec  = 0;
        juce::int64  startUsec = 0;
        juce::String name;
        bool         degraded  = false;
    };
    static const HostIdentity& getHostIdentity();   // resolved once per process

    // ---- Session project name (both hosts) --------------------------------
    // Published to session_project.json whenever a user sets/edits the
    // project name in any instance; mtime-watched by all instances (same
    // pattern as popout_only.txt). Session-scoped FOR REAL: the file is
    // stamped with the publishing host's identity, and the getter returns
    // empty unless the stamp matches the current host — a value from a
    // previous DAW session is invisible, so fresh instances prompt instead
    // of silently adopting it. Precedence lives with the CALLERS: an
    // instance's own serialised name always wins over this value.
    static juce::String getSessionProjectName();
    static void publishSessionProjectName(const juce::String& name);

    // Session genre — separate session_genre.json (NOT folded into the
    // project file: separate mtimes keep the two follow-watchers
    // independent). Same publish/adopt/follow pattern AND the same host
    // identity stamp/match rule; adoption is keyed off the genre-answered
    // FLAG in the callers, never the value (genre has a non-empty default).
    static juce::String getSessionGenre();
    static void publishSessionGenre(const juce::String& genre);

    // Session AUTO-PROJECT name (v2.17): the "Untitled, <date>" project that
    // unnamed chats join for THIS session. Host-stamped like the others so a
    // new DAW session gets a fresh empty getter (and picks a disambiguated
    // name); reopened instances of the SAME session read back the stored
    // name (no fragmentation, no re-disambiguation). Empty until set.
    static juce::String getSessionAutoProject();
    static void setSessionAutoProject(const juce::String& name);

    // If `d` is a popout-only AudioUnit and a VST3 build of the same plugin
    // can be found, return the VST3 desc instead (in-process editor —
    // containable by the existing machinery). Applies at NEW instantiation
    // only; restores keep their saved format. Logs the substitution.
    juce::PluginDescription preferInlineHostableDesc(const juce::PluginDescription& d);

    // VST3 build of `pluginName`: direct entry, previously deep-scanned
    // cache, or on-demand enumeration of WaveShell VST3 modules (a single
    // .vst3 containing many plugins; the thin scan records only the shell).
    // Variant-aware: AU "X (m)"/"(s)" matches VST3 "X Mono"/"X Stereo".
    juce::PluginDescription findVst3Alternative(const juce::String& pluginName);

    // ---- Additive accessors (used by EchoJay Link hosting; main plugin
    // behaviour unchanged) ------------------------------------------------
    juce::PluginDescription getSlotDescription(int i) const;
    juce::AudioProcessor*   getSlotProcessor(int i) const;

    // Sum of the non-bypassed hosted plugins' reported latencies (message
    // thread). Link mirrors this into setLatencySamples on every change.
    int getTotalLatencySamples() const;

    // Fired on the message thread at the end of every graph rebuild
    // (load / remove / move / bypass). Both hosts mirror chain latency into
    // setLatencySamples from here.
    std::function<void()> onChainChanged;

    // ---- State persistence -----------------------------------------------
    void saveToDisk() const;
    void loadFromDisk();
    // Slot IDENTITY only (description, bypass, wet, master wet). This string
    // is byte-for-byte what every build since v2.4.0 has written, and it must
    // stay that way: an older build reading a session saved by a newer one
    // parses exactly the XML it writes itself. Hosted plugin settings ride
    // alongside in a SEPARATE top-level key (see getCachedSlotStatesVar),
    // which an older build ignores because every restore read is
    // hasProperty-gated.
    juce::String getSlotsStateXml() const;
    // slotStates: the sibling object from the session, {"1": "<base64>", ...},
    // 1-based, absent/null meaning "nothing was saved for that slot". Empty
    // var = a session written before hosted settings were persisted, which
    // restores exactly as it always did.
    // slotParams (17 Aug 2026): a second sibling object, {"1": "id=v,id=v,...", ...},
    // the JUCE-side parameter values of VST3 slots, applied AFTER the blob;
    // see getCachedSlotParamsVar. Absent = nothing, exactly the old restore.
    void tryRestoreSlotsFromXml(const juce::String& xml,
                                const juce::var& slotStates = {},
                                const juce::var& slotParams = {});

    // ---- Hosted plugin settings: CACHE, not capture -----------------------
    // getStateInformation on a hosted plugin can take seconds (samplers,
    // convolution) and is not something to run inside the host's own save
    // callback, which is a teardown-adjacent path with its own freeze
    // history. So the settings are serialised AHEAD of the save, on the
    // message thread, and the host's callback only writes strings already
    // held here. The mutex below is EchoJay's alone and is never held across
    // a call into a hosted plugin: capture happens outside it, the lock is
    // taken only to swap the resulting string in.
    //
    // Off by default so EchoJay Link, which shares this class but does its
    // own live capture in LinkProcessor::chainModelToVar, is unaffected.
    void setStateCacheEnabled(bool shouldBeEnabled);
    // Capture now, for any slot that is due (dirty or swept) and past its
    // backoff. Cheap and usually a no-op: a slot nothing has touched since
    // its last capture is skipped, so calling this on every editor teardown
    // costs nothing when nothing changed.
    void refreshStateCacheIfIdle();
    // Capture EVERY slot right now, ignoring the debounce, the sweep gate and
    // the per-slot backoff. For DELIBERATE user actions only, currently the
    // explicit Save.
    //
    // The cache exists so the host's getStateInformation stays fast on quit,
    // where nobody asked for anything and a slow callback is a freeze. It is
    // not a budget to spend on an action the user just took: a knob moved a
    // second before Save would otherwise be saved at its previous value, and
    // the save would look completely successful. Correctness beats latency
    // when someone pressed a button and is waiting for it.
    void captureAllSlotStatesNow();
    // {"1": "<base64>", "2": null} for a consumer that wants the settings.
    // Serialises the cache and nothing else: it never calls into a hosted
    // plugin, so it is safe inside getStateInformation. Nulls any slot over
    // maxSlotBytes, then applies maxTotalBytes largest-dropped-first, and
    // names anything it drops in a note using `consumer` ("this session",
    // "this saved chain").
    //
    // THE CAPS ARE THE CALLER'S, NOT THIS FUNCTION'S. There is exactly one
    // capture path feeding one cache; the session and the API differ only in
    // the numbers they ask for here. See the two cap pairs below.
    juce::var getCachedSlotStatesVar(int maxSlotBytes,
                                     int maxTotalBytes,
                                     const juce::String& consumer) const;

    // VST3 PARAMETER VALUES BESIDE THE BLOB (17 Aug 2026). JUCE delivers a
    // hosted VST3's parameter edits to the plugin's processor INSIDE
    // processBlock (inputParameterChanges), and getStateInformation reads the
    // processor side; Logic idles a stopped channel, so an edit made with the
    // transport stopped is in JUCE's cache (AudioProcessorParameter::getValue,
    // the EDITED value) and not yet in the blob (the STALE value), and a save
    // then a reopen came back at the old settings. Measured live both ways
    // (HANDOVER/measurements/state_test.cpp). So for VST3 slots the cache
    // also holds "id=value,..." (Vst ParamID, stable across builds, unlike
    // index) read from getValue at capture, and restore applies it AFTER
    // the blob through setValueNotifyingHost, which writes the cache and
    // reaches the controller now and the processor at the next process call.
    // Parameters are a subset of state, so applying them last cannot lose
    // non-parameter content (a sampler's file). What it can still get
    // wrong: a plugin that moves a parameter internally without telling the
    // host has that one value overwritten by the cache's older reading.
    // AU slots are never written (correct today, unchanged). Absent = do
    // nothing, so sessions and chains saved before this restore as before.
    // {"n": "id=v,..."} for VST3 slots that have one; void when none. A
    // sibling of the state object in the session (chainSlotParams) and in
    // the share body (stateParams; the SERVER must accept, store and return
    // that key for shares to carry it, see the 17 Aug report).
    juce::var getCachedSlotParamsVar() const;

    // Session-scoped, plain-language notes about settings that did NOT
    // capture or did NOT restore, named by plugin. Never persisted, never
    // silent: a slot sitting at its defaults because its blob was too large
    // or was rejected must say so rather than look restored.
    juce::StringArray getStateNotes() const;
    void clearStateNotes();
    // A save that FAILED, recorded in the same persistent band as the
    // capture/restore notes. Public because the failure is the editor's to
    // report; the note lives here so it survives the editor being recreated
    // (Logic does that on every Link window switch).
    void addSaveFailureNote(const juce::String& note) { addStateNote(note); }

    // ---- Saved chains (B.1) ----------------------------------------------
    // The `slots` array in the shared chain format: 1-BASED and contiguous n,
    // plugin name required, params null until Phase 2 (the key exists from
    // day one so the schema never changes). The server rejects anything else
    // outright, so this is the one place that shape is built.
    //
    // NOT included: `state`. It is a separate top-level field so a share can
    // serialise slots and physically cannot pick up settings by accident.
    // Ask getCachedSlotStatesVar for that, with the API caps.
    juce::var buildChainSlotsVar() const;

    // Load a saved chain (B.2). Resolves each slot's plugin by name, then
    // hands the result to the SAME restore path a session reload takes:
    // state carried ON the item and applied inside that slot's own load
    // callback, per-slot failures named rather than silent. Nothing about
    // restoring settings is duplicated for this route.
    //
    // Does NOT clear the rack. The caller closes hosted editors first and
    // clears a runloop turn later (the AMEK editor-close discipline), which
    // is what the AI chain build already does.
    // onSlotSettled fires on the message thread after EVERY slot attempt,
    // success or failure, so the rack UI can show the chain filling in.
    // Plugin instantiation blocks the message thread, so without this the
    // panel would sit on an empty rack and then jump.
    // isDisabledByName: the Settings disabled-set check, passed as a
    // predicate because the scanner (the one authority for "user unticked
    // it") lives on the editor, not here. resolveByName is scan-wide, so
    // without this a saved chain silently resurrects a plugin the user
    // disabled; the AI build path has always refused these, and the two
    // paths must agree on what is loadable. Called with the RESOLVED name.
    // Empty predicate = no check (session restore keeps its old behaviour).
    void restoreSavedChain(const juce::var& slotsArr, const juce::var& stateObj,
                           std::function<void()> onSlotSettled = {},
                           std::function<bool(const juce::String&)> isDisabledByName = {},
                           const juce::var& paramsObj = {});   // stateParams, keyed by saved n; see getCachedSlotParamsVar

    // TWO CAP PAIRS, DELIBERATELY DIFFERENT. Both in decoded bytes.
    //
    // SESSION: written into the user's project file on every save. Was 128 KB
    // / 512 KB, a number with nothing measured behind it; raised 17 Aug 2026
    // to 4 MB / 16 MB after a 1.1 MB sampler state was refused while Logic
    // projects carry multi-MB plugin states routinely. What it still costs:
    // project-file growth per Cmd-S (about 1.33x the state in base64) and a
    // slower capture tick for that slot, which the backoff already handles.
    // This cap protects a document.
    //
    // API: mirrors MAX_STATE_BYTES_PER_SLOT / MAX_STATE_BYTES_TOTAL in the
    // backend's lib/dash/chains.js, which rejects with 413 rather than
    // truncating. Capping first here is what turns a failed save into an
    // honest partial one. It cannot follow the session cap: the platform's
    // request body limit is 4.5 MB and one 4 MB slot base64-encodes to about
    // 5.4 MB, so it would fail before our 413. This cap protects a request.
    //
    // Do NOT "fix" the difference by aligning them, in either direction. Same
    // note lives in SESSION_B_BUILD_SPEC.md section 5.
    static constexpr int kSessionStateMaxSlotBytes  = 4 * 1024 * 1024;
    static constexpr int kSessionStateMaxTotalBytes = 16 * 1024 * 1024;
    static constexpr int kApiStateMaxSlotBytes      = 256 * 1024;
    static constexpr int kApiStateMaxTotalBytes     = 1024 * 1024;
    // The cache stores up to the LOOSEST consumer's per-slot cap
    static constexpr int kStateStoreMaxSlotBytes    = kSessionStateMaxSlotBytes > kApiStateMaxSlotBytes
                                                        ? kSessionStateMaxSlotBytes : kApiStateMaxSlotBytes;

    bool chainWarningDismissed = false;

    // ---- Public helpers for static pollVST3Validation free function ------
    // (Free functions cannot access private members in C++.)
    // The ONE instantiate site (17 Aug 2026): every route that creates a
    // hosted plugin instance goes through here, under a death mark that is
    // held through cb (see the death-marker block in ChainHost.cpp).
    void asyncCreatePlugin(const juce::PluginDescription& d,
        std::function<void(std::unique_ptr<juce::AudioPluginInstance>, const juce::String&)> cb);
    void completeLoad(std::unique_ptr<juce::AudioPluginInstance> inst,
                      const juce::PluginDescription& desc);
    // reason travels into chain_blacklist.txt next to the path with an ISO
    // date; the first reason recorded for a path wins (it names the
    // original event, later duplicates are re-detections).
    void addToBlacklist(const juce::String& path, const juce::String& reason = {});

    double sampleRate_ = 44100.0;  // public so pollVST3Validation can read
    int    blockSize_  = 512;

private:
    struct ChainSlot {
        juce::AudioProcessorGraph::Node::Ptr node;
        juce::PluginDescription              desc;
        bool                                 bypassed = false;
        juce::String                         settings;   // AI-suggested dial-in guidance
        // Per-slot wet/dry: `wet` is the persisted value; `wetShared` is the
        // audio-thread copy read by this slot's SlotWetBlend graph node.
        // Both created lazily in rebuildGraph(), removed in removeSlot().
        float                                    wet = 1.0f;
        std::shared_ptr<std::atomic<float>>      wetShared;
        juce::AudioProcessorGraph::Node::Ptr     blendNode;
        // Auto-parameter-mapping state
        juce::var                            structuredSettings;        // settings_structured from the chain reply
        juce::String                         fp;                        // fingerprint (computed at load)
        bool                                 structuredApplied = false; // one-shot guard
        DialStatus                           dialStatus = DialStatus::none;
        juce::StringArray                    dialManual;                // unwritten control labels
        juce::StringArray                    dialReadbackMiss;          // wrote wrong, reverted
        juce::StringArray                    dialUnconfirmed;           // written, display stale (bridged)
        int                                  dialAppliedCount = 0;
        // Stale-map ladder (12 Aug 2026): the superseded index fp when the
        // load diverged from the index ("" = no divergence), and whether the
        // rung has been announced. staleIndexedFp stays set on the unmapped
        // rung so the editor can compose the card wording and the
        // suggest-an-alternative pill; it clears on the dialled rung, where
        // there is nothing to say.
        juce::String                         staleIndexedFp;
        bool                                 staleSettled = false;
        juce::StringArray                    dialOutOfRange;   // asked outside the live map's range, refused per value
        // Hosted settings cache (see setStateCacheEnabled). The blob and its
        // bookkeeping are read under stateCacheMutex_; everything else on
        // this struct follows the existing message-thread-only rule.
        juce::String                         lastKnownState;      // base64, empty = none held
        int                                  lastKnownBytes = 0;  // DECODED size of the above
        juce::String                         lastKnownParams;     // VST3 only: "id=value,..." from JUCE's parameter cache (see getCachedSlotParamsVar)
        double                               lastCaptureMs = 0.0; // cost of the last capture
        double                               capturedAtMs  = 0.0; // when it was taken (0 = never)
        double                               nextCaptureMs = 0.0; // backoff gate
        bool                                 oversizeReported = false;  // note said once, not per tick
    };

    // Auto-parameter-mapping caches (message thread only)
    std::map<juce::String, juce::var>    paramMaps_;     // fp -> map object
    std::map<juce::String, juce::String> identityToFp_;  // format|uid|version -> fp
    juce::StringArray                    mapsRequested_; // fps requested this session
    // fps whose fetch is IN FLIGHT (requested, no storeParamMaps answer
    // yet). Distinct from mapsRequested_, which is never cleared (it is the
    // don't-re-request guard). Drives DialStatus::pending vs noMap.
    juce::StringArray                    pendingMapFps_;
    bool                                 mapsRevalidated_ = false; // once-per-session cache revalidation
    // TTL-on-use: epoch-ms of the last server confirm per fp. A cached map
    // older than the staleness bound is refetched before it can dial, so a
    // since-corrected map (the AMEK suppression class) is never applied.
    std::map<juce::String, juce::int64>  fpFetchedAt_;
    std::shared_ptr<int>                 life_ { std::make_shared<int>(0) }; // weak-guard for deferred callbacks
    bool mapFresh (const juce::String& fp) const;
    void refetchStale (const juce::String& fp);
    // WHY THIS TAKES A TRIGGER (10 Aug 2026). The NO SETTINGS line was read as
    // the cause of "nothing dials" and it is not evidence of anything: this
    // function is called at LOAD completion (completeLoad, and the built-in
    // add), while the caller attaches settings in the load callback AFTERWARDS.
    // So every slot of every healthy build passes through here once with void
    // settings and printed the failure line. The two map-arrival sweeps call it
    // for EVERY slot including ones that never had settings, printing it again.
    //
    // A void is only interesting if it is not one of those. The trigger says
    // which call this is, so the log can tell "not attached YET" (ordering,
    // expected, benign) from "settings never arrived for a slot that is ready
    // to take them" (the real fault).
    enum class DialTrigger
    {
        slotLoaded,        // completeLoad / built-in add: settings come next
        settingsAttached,  // setSlotStructuredSettings: settings are here now
        mapArrived,        // storeParamMaps sweep: indiscriminate over slots
    };
    static const char* dialTriggerName (DialTrigger t);
    void applyStructuredIfReady (int slotIndex, DialTrigger trigger);
    // Stale-map ladder resolution: called from the storeParamMaps sweep once
    // the live-fp fetch has answered. Returns true when the card changed
    // (the unmapped rung speaks).
    bool settleStaleRung (int slotIndex);
    // VST3 identity capture, option 1 (13 Aug 2026): fill thin VST3 entries
    // (uniqueId 0 / empty version) from load-validated descriptions held in
    // knownPlugins_. Identity only - name and fileOrIdentifier are the
    // resolver's matching keys and stay untouched. Skips bundle paths that
    // several captured descriptions share (the WaveShell shape), rather
    // than stamping the shell row with an arbitrary member's identity.
    // Returns the number of rows filled. Callers unlatch hasResolved_ when
    // it is nonzero so the resolver rebuilds with the new identities.
    int enrichThinVst3EntriesFromKnown();
    // Construct + append a built-in node synchronously. Returns an error
    // string, empty on success. Called only from loadPluginAsync.
    juce::String loadBuiltinNow (const juce::PluginDescription& desc);
    // Exact apply for ANY built-in slot. Hands the WHOLE settings_structured
    // value to the device's own funnel, which owns its schema — the array form
    // (eq_bands) for a structured device, the flat `params` map for everything
    // else. Returns a summary, or empty when the value carried nothing that
    // device understands. appliedOut/skippedOut count whatever the device counts
    // (bands, or params) — a settings-only move legitimately applies zero of
    // them and still returns a summary.
    // deviceMissingOut separates the two ways this returns an empty string
    // (10 Aug 2026). Both read as "the device understood nothing", and they
    // are opposite faults: no EedDeviceProcessor on the slot at all is a
    // ROUTING/cast failure, while a device that resolved neither of its two
    // accepted shapes is a PAYLOAD failure. Without this the cast failure
    // hides behind the payload one.
    juce::String applyStructuredToBuiltinSlot (int slotIndex, const juce::var& structured,
                                               int* appliedOut = nullptr,
                                               int* skippedOut = nullptr,
                                               bool* deviceMissingOut = nullptr);
    void loadParamMapsFromDisk();
    void saveParamMapsToDisk();
    // Read-only merge of the opt-in background mapper's output file
    // (param_maps_bootstrap.json, written by ejextract --bootstrap).
    // mtime-guarded; called at construction and from requestMapPrefetch.
    void mergeBootstrapMaps();
    // Union the out-of-process catalogue (ejextract --catalogue, P16) into the
    // DAW's state at READ time: chain_plugins_scan.xml (validated identities)
    // into knownPlugins_, chain_fp_scan.json (identityToFp) into the fp index.
    // One file per writer, unioned here, so the helper and the DAW never race.
    void loadHelperCatalogue();
    // Per-plugin supersession across shell versions: for entries that are the
    // SAME plugin (format|uid|manufacturer, the same key the map lookup uses,
    // NOT the display name) at more than one version, mark all but the newest
    // superseded. "Superseded" means only "a strictly newer copy of this exact
    // plugin exists here"; a plugin present at one version is never superseded.
    // Nothing is deleted; the picker keeps everything.
    void computeSupersessions();
public:
    // Per-bundle health written by the out-of-process catalogue
    // (chain_health.json, keyed by fileOrIdentifier). The withheld panel reads
    // the FAILURE states (crashed, timed-out, load-failed, load-failed-licence,
    // shell); loaded-not-verified is stored but deliberately NOT surfaced,
    // since marking the whole working library "not verified" before a verified
    // state exists would libel it.
    struct HealthEntry { juce::String state, reason; juce::int64 blockMs = 0; };
    std::map<juce::String, HealthEntry> getHealthSnapshot() const;
private:
    std::map<juce::String, HealthEntry> health_;   // fileOrIdentifier -> entry
    void reloadHealthFromDisk();
    // Superseded by identityKey (format|uidHex|version). Read by the withheld
    // panel and, once the server tier lands, by the feed.
    std::set<juce::String> superseded_;
public:
    bool isSuperseded(const juce::PluginDescription& d) const;
private:
    juce::Time bootstrapMergedMtime_;

    // Fingerprint pass state (message thread only, except the atomic)
    std::atomic<bool>                    fpPassRunning_ { false };
    juce::Array<juce::PluginDescription> fpQueue_;
    int                                  fpQueueTotal_ = 0;
    juce::StringArray                    fpAttempted_;  // crash/failure skip markers, persisted
    void fingerprintNext();

    juce::AudioPluginFormatManager formatManager_;

    // Plugin list (from scan)
    mutable std::mutex  pluginsMutex_;
    juce::Array<juce::PluginDescription> entries_;
    juce::KnownPluginList                knownPlugins_;
    juce::StringArray                    blacklist_;
    std::map<juce::String, int>          stateOversize_;   // path -> default-state bytes; see WithholdReason::SettingsTooLarge
    void reloadStateOversizeFromDisk();                    // pluginsMutex_ taken inside
    void recordStateOversize(const juce::String& path, int bytes, const juce::String& name, const juce::String& where);
    // Replaces the in-memory set from chain_blacklist.txt (startup and every
    // scan): the file is the authority, so deleting a line re-enables the
    // plugin on the next scan without a host restart.
    void reloadBlacklistFromDisk();
    // path -> "reason<TAB>ISO date", written into chain_blacklist.txt after
    // the path. Absent for entries that predate the tabbed format; the
    // reader tolerates bare paths and the writer keeps them bare. NOT a
    // fourth exclusion store: the blacklist is still blacklist_, this only
    // annotates it.
    juce::StringPairArray                blacklistMeta_;

    // AudioProcessorGraph: input → [active slots in order] → output
    std::unique_ptr<juce::AudioProcessorGraph> graph_;
    juce::AudioProcessorGraph::Node::Ptr inputNode_, outputNode_;

    std::vector<ChainSlot> slots_;

    // Removed slots' nodes — kept ALIVE (disconnected, not processed) because
    // some plugins leak UI timers that fire into their AudioUnit after editor
    // close; disposing the instance makes that a use-after-free crash.
    // Freed only when the ChainHost itself is destroyed.
    std::vector<juce::AudioProcessorGraph::Node::Ptr> graveyard_;

    bool   prepared_  = false;
    std::atomic<bool> hasActiveSlots_ { false };  // true when ≥1 non-bypassed slot exists
    std::atomic<int>  chainRevision_ { 0 };       // see getChainRevision()
    // Every chain mutation is also a settings-cache trigger: this is the
    // "after a chain edit settles" refresh point, reached through the same
    // debounce as everything else, so a burst of edit ops captures once at
    // the end rather than once per op.
    void bumpChainRevision() noexcept { chainRevision_.fetch_add(1, std::memory_order_relaxed);
                                        noteHostedChange(); }

    // Running level at the chain input and output (see getChainInLevels):
    // K-weighted (LUFS), the perceived level C7 matches
    echojay::LevelTally chainInTally_  { echojay::LevelTally::Weighting::K };
    echojay::LevelTally chainOutTally_ { echojay::LevelTally::Weighting::K };
    double              tallySr_ = 0.0;   // rate the tallies were prepared at
    juce::String hostTrackName_;
    juce::String hostPluginFormat_;   // see setHostPluginFormat
    juce::String restoredLevelsTrack_;   // the track a restored tally was measured on, until the host names this one
    // Pending per-slot level restore, keyed by saved slot number (1-based,
    // the same key the hosted-state object uses); consumed by restoreNextSlot
    std::map<int, std::pair<juce::var, juce::var>> pendingSlotLevels_;
    // Which node fed each slot at the last rebuild: a slot whose predecessor
    // changed (move, insert before it, the previous slot bypassed) sees a
    // different signal, so its tallies restart.
    std::vector<juce::AudioProcessorGraph::NodeID> builtPredecessors_;

    // ---- Master wet/dry state (audio thread reads, message thread writes) --
    // dryRing_ holds the pre-graph input so the dry leg can be delayed by the
    // chain's reported latency before blending — wet and dry stay
    // sample-aligned. Sized for ~5s of lookahead/linear-phase latency.
    static constexpr int kDryRingLen = 1 << 18;
    std::atomic<float>          masterWet_ { 1.0f };
    juce::SmoothedValue<float>  masterWetSmooth_;

    // Pre-chain gain (see the pre-gain block above). preGainDb_ is read by
    // the audio thread; preGainUserSet_ / preGainState_ are message-thread.
    std::atomic<float>          preGainDb_ { 0.0f };
    juce::SmoothedValue<float>  preGainSmooth_;
    bool                        preGainUserSet_ = false;
    PreGainState                preGainState_   = PreGainState::Off;
    juce::AudioBuffer<float>    dryScratch_;   // delayed-dry read buffer (blockSize)
    juce::AudioBuffer<float>    dryRing_;      // dry history ring (2 x kDryRingLen)
    int                         dryRingWrite_ = 0;

    // Resolver cache (message thread only — no mutex needed)
    std::vector<RecommendableEntry> recommendable_;
    int recommendableEnabledIn_ = 0;   // how many enabled scanner entries were fed in
    juce::String recommendableFormat_; // filter used for the last build (fallback resolves honour it)
    bool hasResolved_ = false;         // latched by buildRecommendable once inputs were real

    // Entries-cache staleness tracking (message thread)
    juce::Time entriesCacheTime_;
    juce::int64 entriesScannedAtMs_ = 0;   // see getEntriesScannedAtMs()
    juce::int64 scanStartedAtMs_    = 0;   // set when startScan accepts; ages a rejected press

    // Session-scoped load failures (see the comment block in the public
    // section): normalised "name|format" keys, message thread only,
    // intentionally lost on plugin reload.
    juce::StringArray sessionLoadFailed_;

    // Scan thread
    std::atomic<bool>  scanning_     { false };
    std::atomic<bool>  cancelFlag_   { false };
    std::atomic<float> scanProgress_ { 0.0f };
    mutable std::mutex statusMutex_;
    juce::String       scanStatus_;
    std::thread        scanThread_;

    // Edit sequencer internals (Phase 1c) — see applyChainEdits
    void runNextEditOp(std::shared_ptr<void> stateErased);
    void walkSlotTo(std::vector<int>& map, int fromCur, int toCur);

    // Restore helper (sequential async restore of multiple slots).
    // stateBase64 is carried ON THE ITEM and applied inside that item's own
    // load callback, never looked up by final slot index afterwards: a slot
    // that fails to load shifts every later slot down, and an index lookup
    // would then write slot 3's settings into slot 4. Same shape as
    // LinkProcessor's ChainBuildItem, which solved this first.
    // expectState: the session carried a settings object for the chain, so a
    // slot with nothing in it is a slot whose settings we did not save,
    // which the user is told about. False on every pre-existing session,
    // where there is nothing to have lost and a note would be pure noise.
    struct RestoreItem { juce::PluginDescription desc; bool bypassed; float wet = 1.0f;
                         juce::String stateBase64; bool expectState = false;
                         juce::var params; };   // the {uid,format,plugin,params} object for a VST3 slot (see getCachedSlotParamsVar), void = none
    // onSlotSettled: see restoreSavedChain. Empty for a session restore,
    // which builds its rack before any editor exists to watch it.
    void restoreNextSlot(std::vector<RestoreItem> items, int idx,
                         std::function<void()> onSlotSettled = {});
    void applyRestoredState(int slotIdx, const juce::String& b64,
                            bool expectState, const juce::String& slotName);
    // AFTER applyRestoredState. The per-slot {uid,format,plugin,params} object;
    // applied only when the resolved slot IS that plugin (uid + format). See
    // getCachedSlotParamsVar.
    void applyRestoredParams(int slotIdx, const juce::var& params, const juce::String& slotName);

    // ---- Hosted settings cache internals ---------------------------------
    // Debounce: capture 2s after the last observed change, so one knob drag
    // (which emits changes every few milliseconds) collapses into a single
    // capture, while a Cmd-S essentially never lands inside that window.
    // Sweep: plugins that never notify a change would otherwise never be
    // recaptured, so every slot is re-read on a slow cycle regardless.
    // Backoff: a slot whose capture proved expensive earns a long minimum
    // interval, so one sampler cannot make the whole cache thrash.
    static constexpr int    kStateTickMs        = 500;
    static constexpr double kStateDebounceMs    = 2000.0;
    static constexpr double kStateSweepMs       = 15000.0;
    static constexpr double kStateExpensiveMs   = 250.0;   // capture cost that earns backoff
    static constexpr double kStateBackoffFactor = 20.0;
    static constexpr double kStateOversizeMs    = 30000.0; // retry gate for a capped slot

    struct StateCacheTimer;
    std::unique_ptr<StateCacheTimer> stateCacheTimer_;
    mutable std::mutex               stateCacheMutex_;
    bool                             stateCacheEnabled_ = false;
    // A hosted getStateInformation can spin a modal loop of its own, which
    // dispatches timers and would otherwise let a capture re-enter itself.
    bool                             inStateCapture_ = false;
    std::atomic<double>              lastChangeMs_ { 0.0 };  // any hosted change, any thread
    std::atomic<bool>                stateDirty_   { false };
    std::atomic<int>                 hostedEpoch_  { 0 };    // see getHostedChangeEpoch()
    mutable juce::StringArray        stateNotes_;            // guarded by stateCacheMutex_

    void  stateCacheTick();
    void  captureSlotState(int i, double nowMs);
    // const because the save path records the notes for anything the total
    // cap drops, and that path is const.
    void  addStateNote(const juce::String& note) const;
    void  noteHostedChange() noexcept;      // callable from any thread
    // RENAMED from attachStateListener/detachStateListener: the listener is
    // the SIGNAL, the state cache is only one consumer of it. It used to
    // refuse to attach unless the state cache was on, which meant the Link
    // (which never enables that cache) could not see a hosted knob move at
    // all. See attachHostedListener's body.
    void  attachHostedListener(int i);
    void  detachHostedListener(int i);

    // juce::AudioProcessorListener: the hosted plugins tell us when
    // something moved. Both can arrive on the audio thread during
    // automation, so they touch nothing but atomics: no container access,
    // no allocation, no lock. A LATENCY change additionally schedules a
    // graph rebuild (see LatencyRebuilder): the render sequence bakes each
    // plugin's latency into the dry-leg delays at build, so a plugin that
    // changes it at runtime (an oversampling / HQ switch) left every
    // partial-wet blend comb-filtering and the host's reported latency
    // stale until the next structural change (measured 16 Aug 2026,
    // HANDOVER/measurements/slot_wet_null_test: residual -3 dB, comb to
    // -12 dB, cured by a rebuild). Only latencyChanged schedules it; a
    // parameter move never does.
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override
        { noteHostedChange(); }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override
        { noteHostedChange(); if (d.latencyChanged) onHostedLatencyChanged(); }
    // Message-thread, coalesced: triggerAsyncUpdate from any thread, an 80 ms
    // debounce so a plugin that fires twice for one switch rebuilds once, and
    // a compare against the latencies the graph was last built with so a
    // no-op notification rebuilds nothing.
    struct LatencyRebuilder;
    std::unique_ptr<LatencyRebuilder> latencyRebuilder_;
    std::vector<int>                  builtLatencies_;   // per slot at the last rebuildGraph (message thread)
    void onHostedLatencyChanged() noexcept;   // any thread
    void rebuildForLatencyIfChanged();        // message thread

    // Internal helpers
    void rebuildGraph();
    void doRefresh();
    void setScanStatus(const juce::String& s);
    bool isBlacklisted(const juce::String& path) const;

    // Architecture gate for VST3 rows (15 Aug 2026). A VST3 bundle whose
    // binary carries no slice for the RUNNING process (x86_64-only under a
    // native arm64 host: 331 of 449 on the census machine) is offered,
    // resolved and fed to the model, then fails at load with "No types
    // found". Consulted at FEED time beside the format check, exactly as
    // blacklist_ is, and never at scan time: the entries cache is shared
    // between the AU and VST3 hosts and the answer depends on the process
    // (native vs Rosetta), so a scan-time drop would be right for whichever
    // host scanned and wrong for the other. Memoised per path in memory
    // only, never persisted; cleared when a scan re-reads the folders.
    // A read that fails for ANY reason (no binary, unknown magic, short
    // file) is Unreadable and KEPT: failing closed produces a silently
    // missing plugin, which is the defect this exists to fix.
    // archMutex_ is its own lock because every caller already holds
    // pluginsMutex_ (non-recursive).
    enum class ArchVerdict { Loadable, NotLoadable, Unreadable };
    ArchVerdict archVerdict(const juce::String& path) const;
    bool archLoadable(const juce::String& path) const;   // Unreadable counts as loadable
    mutable std::mutex                          archMutex_;
    mutable std::map<std::string, ArchVerdict>  archCache_;
    // The one decision (see WithholdReason, public). Caller holds
    // pluginsMutex_: blacklist_ is read directly, archVerdict takes its own
    // lock. The three feed sites call THIS; the public withholdReason wraps it.
    WithholdReason withholdReasonLocked(const juce::PluginDescription& d) const;

    juce::AudioPluginFormat* getFormatByName(const juce::String& namePart) const;

    static juce::File getPluginListFile();
    static juce::File getBlacklistFile();
    static juce::File getDeadmanFile();
    // Turn every chain_load_deadman.<pid>.txt left by a dead process into
    // blacklist rows (constructor; see the death-marker block in the cpp)
    void consumeDeathMarks();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainHost)
};
