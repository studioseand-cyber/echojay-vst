#pragma once
#include <JuceHeader.h>
#include "PluginScanner.h"
#include <atomic>
#include <map>
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
    void cancelScan();
    bool  isScanning()      const noexcept { return scanning_.load(); }
    float getScanProgress() const noexcept { return scanProgress_.load(); }
    juce::String getScanStatus() const;

    // ---- Plugin list (message thread) ------------------------------------
    int getNumPlugins() const;
    juce::Array<juce::PluginDescription> getFilteredPlugins(
        const juce::String& filter,
        const juce::String& formatFilter = {}) const;

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
        int               appliedCount = 0;
    };
    std::vector<SlotDialInfo> getDialInfos() const;
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
        juce::String op;        // add | remove | replace | move | bypass
        int  slot  = -1;        // original index (remove/replace/move/bypass)
        int  to    = -1;        // move target (original numbering)
        int  after = -2;        // add: insert after this original slot; -1 = first
        bool on    = false;     // bypass state
        juce::String name;      // add/replace: name from AVAILABLE PLUGINS
        juce::String settings;  // prose settings for the slot tile (display)
        // add/replace: the machine-readable settings_structured that rides the
        // op, applied once the new slot has loaded. Same payload and same
        // consumer as the build path, so an EQ added by an EDIT turn is dialled
        // exactly like one added by a chain build instead of arriving flat.
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

    // EchoJay auto-parameter-mapping: dial a slot's hosted plugin from
    // structured settings plus the plugin's map.
    struct ApplyReport { juce::String semantic; bool applied; float normalized; juce::String note;
                         juce::String landedText; bool displayVerified = false; bool readbackMismatch = false; };
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
    juce::PluginDescription resolveByName(const juce::String& rawName,
                                          const juce::String& formatFilter,
                                          juce::String* matchLogOut = nullptr) const;

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
    static constexpr const char* kBuiltinFormat = "EchoJayBuiltin";
    static constexpr const char* kBuiltinEqName = "EchoJay EQ";

    // Every built-in device the chain can host, by exact name. SINGLE source
    // of truth: the AI feed advertises from here (see
    // EchoJayAPI::buildPluginInjection), and the backend offers a built-in
    // only when its exact name appears in that advertisement — so a future
    // built-in becomes offerable by appending one line here, with no version
    // pin on either side.
    static juce::StringArray builtinDeviceNames()
    {
        return juce::StringArray { juce::String (kBuiltinEqName) };
    }

    // Canonical synthetic description. Stable across machines and sessions:
    // it is what gets written into the saved chain XML and matched on restore.
    static juce::PluginDescription builtinEqDescription();
    static bool isBuiltinDescription (const juce::PluginDescription& d) noexcept;
    static bool isBuiltinEqName      (const juce::String& rawName);

    // True when slot i holds the built-in EQ (used to route exact apply).
    bool isBuiltinEqSlot (int i) const;

    // DEV ONLY. Apply a hand-written eq_bands JSON to a built-in EQ slot,
    // exercising the exact-apply path without the backend (the server
    // contract that emits eq_bands is not deployed yet). Returns a summary,
    // or a human-readable reason it did nothing. Callers gate on
    // devModeActive(); this is inert in a release session because nothing
    // reachable calls it there.
    juce::String devApplyEqJson (int slotIndex, const juce::String& json);

    // Index of the first built-in EQ slot, or -1. Used by the dev command to
    // pick a target when the selected slot is not an EQ.
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
    void tryRestoreSlotsFromXml(const juce::String& xml,
                                const juce::var& slotStates = {});

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
    void restoreSavedChain(const juce::var& slotsArr, const juce::var& stateObj,
                           std::function<void()> onSlotSettled = {});

    // TWO CAP PAIRS, DELIBERATELY DIFFERENT. Both in decoded bytes.
    //
    // SESSION (tighter): an oversized session has no server to reject it. It
    // is written into the user's project file on every single save, it grows
    // the file forever, and there is no undo. This cap protects a document.
    //
    // API (looser): mirrors MAX_STATE_BYTES_PER_SLOT / MAX_STATE_BYTES_TOTAL
    // in the backend's lib/dash/chains.js, which rejects with 413 rather than
    // truncating. Capping first here is what turns a failed save into an
    // honest partial one. This cap protects a request.
    //
    // Do NOT "fix" the difference by aligning them, in either direction. Same
    // note lives in SESSION_B_BUILD_SPEC.md section 5.
    static constexpr int kSessionStateMaxSlotBytes  = 128 * 1024;
    static constexpr int kSessionStateMaxTotalBytes = 512 * 1024;
    static constexpr int kApiStateMaxSlotBytes      = 256 * 1024;
    static constexpr int kApiStateMaxTotalBytes     = 1024 * 1024;

    bool chainWarningDismissed = false;

    // ---- Public helpers for static pollVST3Validation free function ------
    // (Free functions cannot access private members in C++.)
    void asyncCreatePlugin(const juce::PluginDescription& d,
        std::function<void(std::unique_ptr<juce::AudioPluginInstance>, const juce::String&)> cb)
    {
        formatManager_.createPluginInstanceAsync(d, sampleRate_, blockSize_, std::move(cb));
    }
    void completeLoad(std::unique_ptr<juce::AudioPluginInstance> inst,
                      const juce::PluginDescription& desc);
    void addToBlacklist(const juce::String& path);

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
        int                                  dialAppliedCount = 0;
        // Hosted settings cache (see setStateCacheEnabled). The blob and its
        // bookkeeping are read under stateCacheMutex_; everything else on
        // this struct follows the existing message-thread-only rule.
        juce::String                         lastKnownState;      // base64, empty = none held
        int                                  lastKnownBytes = 0;  // DECODED size of the above
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
    void applyStructuredIfReady (int slotIndex);
    // Construct + append a built-in node synchronously. Returns an error
    // string, empty on success. Called only from loadPluginAsync.
    juce::String loadBuiltinNow (const juce::PluginDescription& desc);
    // Exact apply for a built-in EQ slot. Hands the WHOLE settings_structured
    // value to the device's own funnel, which owns the schema (legacy bare
    // eq_bands array, or the object carrying eq_bands / eq_settings / … ).
    // Returns a summary, or empty when the value carried nothing the EQ
    // understands. appliedOut/skippedOut count bands only — a settings-only
    // move legitimately applies zero bands and still returns a summary.
    juce::String applyStructuredToEqSlot (int slotIndex, const juce::var& structured,
                                          int* appliedOut = nullptr,
                                          int* skippedOut = nullptr);
    void loadParamMapsFromDisk();
    void saveParamMapsToDisk();
    // Read-only merge of the opt-in background mapper's output file
    // (param_maps_bootstrap.json, written by ejextract --bootstrap).
    // mtime-guarded; called at construction and from requestMapPrefetch.
    void mergeBootstrapMaps();
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

    // ---- Master wet/dry state (audio thread reads, message thread writes) --
    // dryRing_ holds the pre-graph input so the dry leg can be delayed by the
    // chain's reported latency before blending — wet and dry stay
    // sample-aligned. Sized for ~5s of lookahead/linear-phase latency.
    static constexpr int kDryRingLen = 1 << 18;
    std::atomic<float>          masterWet_ { 1.0f };
    juce::SmoothedValue<float>  masterWetSmooth_;
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
                         juce::String stateBase64; bool expectState = false; };
    // onSlotSettled: see restoreSavedChain. Empty for a session restore,
    // which builds its rack before any editor exists to watch it.
    void restoreNextSlot(std::vector<RestoreItem> items, int idx,
                         std::function<void()> onSlotSettled = {});
    void applyRestoredState(int slotIdx, const juce::String& b64,
                            bool expectState, const juce::String& slotName);

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
    mutable juce::StringArray        stateNotes_;            // guarded by stateCacheMutex_

    void  stateCacheTick();
    void  captureSlotState(int i, double nowMs);
    // const because the save path records the notes for anything the total
    // cap drops, and that path is const.
    void  addStateNote(const juce::String& note) const;
    void  noteHostedChange() noexcept;      // callable from any thread
    void  attachStateListener(int i);
    void  detachStateListener(int i);

    // juce::AudioProcessorListener: the hosted plugins tell us when
    // something moved. Both can arrive on the audio thread during
    // automation, so they touch nothing but two atomics: no container
    // access, no allocation, no lock.
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override
        { noteHostedChange(); }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override
        { noteHostedChange(); }

    // Internal helpers
    void rebuildGraph();
    void doRefresh();
    void setScanStatus(const juce::String& s);
    bool isBlacklisted(const juce::String& path) const;
    juce::AudioPluginFormat* getFormatByName(const juce::String& namePart) const;

    static juce::File getPluginListFile();
    static juce::File getBlacklistFile();
    static juce::File getDeadmanFile();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainHost)
};
