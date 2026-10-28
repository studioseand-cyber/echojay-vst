#pragma once
// =============================================================================
//  EJAgentExecutorRead — the READ-ONLY half of the agent's ToolExecutor.
//
//  What it answers (CONTRACT_AGENT_TOOLS.md 2.1 / 2.3, with Sean's aliases):
//    look  what: rack | get_rack          the rack as the contract shapes it, one line per slot
//          what: channel | list_tracks    this channel {uid, name, kind}, the Links from the registry
//          what: analysis | analyse       the compact analysis (numbers + at most six bands)
//          what: levels                   per-slot in/out/GR and the chain in/out
//          what: inventory                installed plugin names, trimmed to fit the 8 KB rule
//          what: maps | saved_chains      -> error server_tool (the server answers those)
//    check what: level | measure          the window's in/out LUFS and true peak (the loop's reading)
//          what: gr                       one slot's gain reduction
//          what: true_peak                the final limiter's (or a slot's) peak and the overs
//          what: spectrum                 the six bands after the chain
//          what: balance                  this channel against the mix bus
//          what: compare_to_checkpoint    now against a captured checkpoint: rack, level, peak
//    captureCheckpoint                    a snapshot of rack + levels + analysis, keyed by a token
//    beginPlaybackWindow / readPlayback   the loop's window discipline: the loop-owned tallies reset,
//                                         the song's integrated reading NEVER (08c F2-3, Sean's 13:22 rule)
//    doOp / undoStep / undoToCheckpoint   not_in_phase - Session A implements them over the remote-control channel
//
//  HOW IT READS. Everything comes through `Sources`, a set of functions bound
//  once: to the real objects by bindToProcessor() (the local ChainHost and its
//  tallies, the Link registry + rack sidecar + meter frame for a remote rack -
//  LINK_REMOTE_CONTROL_PLAN.md section 5: files plus shared memory, read-only,
//  the only host that sees the audio publishes the figures), or to a fake host
//  by tools/agent_executor_read_guard. The executor itself holds no pointer to
//  a processor, so its shapes and its compactness are testable without one.
//
//  COMPACTNESS (contract 5): never meter arrays; settings strings capped; the
//  inventory trimmed with `truncated:true`; fitUnder8K() is the one gate every
//  result passes through before it leaves.
//
//  Every call completes on the message thread, synchronously.
// =============================================================================
#include <JuceHeader.h>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <vector>
#include "EJAgentTools.h"
#include "EchoJayLevelTally.h"

class EchoJayProcessor;

namespace echojay::agent
{

// ---- what a read returns, as data -------------------------------------------------
struct SlotRead
{
    int   n = 0;                     // 1-based, as the model sees it (contract 8: 1-based everywhere a model sees)
    juce::String name, format, settings, dialSummary, pictureText;
    /** 19 Oct 2026 (his 17:09 run): the slot's LIVE band lines, asked of the EQ itself. Empty when
        the slot is not the built-in EQ, or when this rack is a LINK (the sidecar does not publish
        structured bands yet - see the morning report). A read-back prefers this, then `settings`,
        and NEVER `dialSummary`: that field is the EJDialSummary DEBUG row by construction. */
    juce::String liveBands;
    bool  bypassed = false, keepLevel = false, builtin = false;
    float wet = 1.0f, preTrimDb = 0.0f, outGainDb = 0.0f;
    bool  pictureValid = false, grKnown = false;
    float inLufs = -200.0f, outLufs = -200.0f, inTpDb = -200.0f, outTpDb = -200.0f, grDb = 0.0f;
    // THE LISTEN WINDOW (Sean's checks 2 and 3, 15 Oct 2026). grDb above is a momentary sample, which
    // is what was being reported and is not a measurement of a compressor. grWindowFrames == 0 means
    // NOTHING WAS MEASURED, and the read must say so rather than quote 0 dB.
    float grPeakDb = 0.0f, grTypicalDb = 0.0f, grLoudestDb = 0.0f, grHeldDb = 0.0f;
    /** SEAN'S RULING, 15 Oct 2026: NO ESTIMATES, EVER. A figure is given only for an EchoJay device or
        a plugin certified in its map (it passed the EJ Maps sweep). Anything else returns
        `not_certified` with no number at all. */
    bool  grCertified = false;
    double grWindowFrames = 0.0;
};

struct RackRead
{
    bool  valid = false;
    juce::String why;                // when ! valid: "no Link with that uid", "sidecar unreadable"
    juce::String uid, name, kind;    // kind: channel | bus | mix_bus | master
    bool  remote = false;
    int   revision = -1;
    float masterWet = 1.0f, preGainDb = 0.0f;
    std::vector<SlotRead> slots;
    /** ---- ITEM 13 (18 Oct 2026): CAN THIS RACK BE MEASURED, AND HOW OLD IS THE READING? -------
        For the open rack both are implicit - the measurement is this process's own and it is now.
        For a LINK they are facts that have to travel: meterCapable false means that Link's build
        does not publish its meters (a different sentence from "a Link cannot be measured"), and
        measuredAtMs is when it last did, so a stale window is refused rather than quoted. */
    bool  meterCapable = true;
    juce::int64 measuredAtMs = 0;
};

struct TrackRead
{
    juce::String uid, name;
    bool  connected = false, audioFlowing = false, active = true, fresh = true;
    // 18 Oct 2026 (queue item 13): the Link's own word that it is REBUILDING its rack. A rebuilding Link
    // answers nothing for a few seconds, which is indistinguishable from death if you only look at the
    // heartbeat - and "it shows as gone" about a track whose chain the user is watching being built is the
    // defect this field exists to end. busy is reported INSTEAD of gone, never alongside it.
    bool  busy = false;
    int   channels = 0, placement = 0;   // placement 0 unset, 1 bus, 2 insert
    float gainDb = 0.0f;
};

struct AnalysisRead
{
    bool  valid = false;
    juce::String why;                // when ! valid
    float integratedLufs = -200.0f, shortTermMaxLufs = -200.0f, truePeakMaxDb = -200.0f, psrDb = -999.0f, lraLu = 0.0f;
    int   oversCount = 0;
    bool  haveBands = false;
    std::array<float, 6> bandRelDb {};   // sub, low, lowMid, mid, highMid, air - dB vs the six-band mean
    float heardSeconds = 0.0f;
    bool  playing = false, transportKnown = false;
    juce::uint32 ageMs = 0;          // a Link's latched frame age; 0 for the own channel
};

// THE LEVELLING RECORD, AT RACK LEVEL (Sean, 10 Oct 2026: the EchoJay Level slot is dropped; levelling drives the
// rack OUT gain for `match` or the final limiter's IN gain for a target, and the record lives on the rack). The
// executor REPORTS it; it never derives it from a slot. Absent fields are NaN / empty and are simply not sent.
struct LevellingRead
{
    bool  present = false;
    juce::String option;             // match | pushed | dynamic (CONTRACT_LEVEL_PARAMS.md's words)
    float targetLufs = std::numeric_limits<float>::quiet_NaN();     // with pushed / dynamic
    juce::String drives;             // "rack_out" (match) | "limiter_in" (a target) | ""
    float landedGainDb = std::numeric_limits<float>::quiet_NaN();   // what the levelling wrote
    float inLufs  = std::numeric_limits<float>::quiet_NaN();        // the chain's recorded in / out integrated loudness
    float outLufs = std::numeric_limits<float>::quiet_NaN();
    bool  converged = false;
    juce::String state;              // the loop's own word when it has one (armed / measuring / proposed / landed / stale)
    juce::int64 updatedMs = 0;
    /** ---- THE LIMITING BUDGET REPORT (Sean, 21 Oct 2026; B's CONTRACT_limit_budget) ---------
        *"If the target can't be reached inside it, stop at the budget, keep the ceiling, and report
        the level reached so B can say so."*

        ---- AND THE NAMES ARE B'S, WHICH COST A CORRECTION --------------------------------------
        My first cut sent the MEASURED limiting as `limitAvgDb` and echoed B's budget back as
        `avgLimitDb`. B's contract reads `avgLimitDb` AS THE MEASUREMENT - its own example is
        `"avgLimitDb": 3.1` against a budget of 3, which is only sensible as what the limiter was
        actually taking. Two fields a word-order apart, meaning different things, is a number that
        would have arrived in the wrong place and read as plausible. So:

          avgLimitDb / peakLimitDb   MEASURED from our own tallies - (the slot's IN level plus the
                                     input gain we wrote) minus its OUT level, short-term over the
                                     loud hops. No plugin meter is read, so they exist for any
                                     limiter. NaN = not measured, and then they are not sent.
          stoppedAtBudget            true only when the budget is what held the push.
          reachedLufs                the level it reached when it did.
          inputDb / limiterSlot      optional in B's contract and cheap here: the input gain the
                                     loop holds and the slot it drives, which is what B's "push
                                     anyway" arithmetic adds to.

        The budget itself is NOT echoed back: B sent it, so repeating it is a second copy of one
        fact - and it was the second copy that produced the name collision. */
    float avgLimitDb   = std::numeric_limits<float>::quiet_NaN();
    float peakLimitDb  = std::numeric_limits<float>::quiet_NaN();
    bool  stoppedAtBudget = false;
    float reachedLufs  = std::numeric_limits<float>::quiet_NaN();
    float inputDb      = std::numeric_limits<float>::quiet_NaN();
    int   limiterSlot  = 0;          // 1-based; 0 = not stated
};

struct Sources
{
    /** FIX 1(b), 14 Oct: ONE SLOT PARAMETER, BY NAME, FOR A READ-BACK. `uid` empty = the open rack, where
        the value comes from the device itself (numeric, exact). A Link's values come from its own sidecar,
        which is the only thing readable across the process boundary. False = not readable, which the caller
        reports as "could not confirm" and never as success. */
    std::function<bool (const juce::String& uid, int slot1Based, const juce::String& paramId, double& out)> slotParam;
    /** ---- CONFIRMED BY POSITION (19 Oct 2026, his 12:31) -------------------------------------
        TRUE when the DIAL confirmed this control as applied even though its display text cannot be
        read as a number. The CL 1B's Threshold is the case: *"APPLIED 0.204 (applied (display
        unverifiable on this plugin)) landed 0.1"* - the applier checks the parameter's own
        normalised round-trip, so the write is verified; what fails is turning the plugin's display
        into a figure. Without this the read-back asked only the display road, got nothing, and
        reported "could not read these values back" about a setting sitting exactly where it was
        put - which he read as "the Threshold never reads back".

        Keyed by semantic, as slotParam is, so the two roads cannot disagree about which control is
        meant. FALSE whenever the answer is not known, never a guess. */
    std::function<bool (const juce::String& uid, int slot1Based, const juce::String& semantic)> slotControlConfirmed;

    /** ---- IS THIS SLOT'S DIAL STILL RUNNING? (16 Oct 2026, the joint live pass) --------------------
        "" uid = the own rack, else a Link's. TRUE while the slot's dial is PENDING: the map has been
        asked for and has not arrived, or the write has not been read back yet.

        The read-back needs it to tell two different facts apart. A third-party add on a Link read back
        `Threshold is 0.00 (asked -18.00)` in two of five live attempts and said "the settings did NOT
        take" - a positive claim that the write failed, made four seconds into a round trip that runs
        V2 -> server -> V2 -> Link -> dial -> sidecar. Unset reads false, which is the settled case and
        the behaviour this build had before the field existed. */
    std::function<bool (const juce::String& uid, int slot1Based)> slotDialPending;

    /** ---- WHAT THE KNOB CAN REPRESENT (18 Oct 2026, Sean in Logic) ----------------------------
        The read-back tolerance follows the control's own ladder: the step the map declares near the
        asked value, in the control's units. 0 (or no hook at all) = the map cannot say, and the
        comparison keeps the tolerance it had. By plugin NAME, because a Link's slot read carries a
        name and not a fingerprint. */
    std::function<double (const juce::String& pluginName, const juce::String& paramId, double near)> paramStep;

    /** ---- WHAT THE SLOT HEARD (18 Oct 2026, Sean's ruling on the GR item) ---------------------
        EchoJay does not read a third party's gain-reduction meter and does not estimate one. What
        it DOES measure is the level arriving at that slot, and that is what check(gr) hands back
        for a third party: the loudest and the typical, as loudness and as true peak, over the
        listen window - with the window's own start time, so B can judge for itself whether the
        reading belongs to the section that was played. B holds the profiles and decides whether a
        figure may be derived from it; the plugin reports the measurement and makes no claim.

        `loudestDb`/`typicalDb` are the p90/p50 of the gated 100 ms hops; the peaks are the same two
        percentiles of those hops' true peaks. known=false means nothing was heard, which is a
        refusal and never a figure. */
    struct SlotInputLevels
    {
        bool  known = false;
        float loudestDb = -200.0f, typicalDb = -200.0f;          // loudness (K) or RMS, as the tally weighs it
        float loudestPeakDbtp = -200.0f, typicalPeakDbtp = -200.0f;
        float windowSeconds = 0.0f;
        bool  kWeighted = false;
        // ---- PLAIN RMS, HOP BY HOP (19 Oct 2026, Sean's test 1) --------------------------------
        // *"Kathy's profiles are sine RMS dBFS, and K-weighting cuts a kick's low end, so the
        // figure would be wrong. Add plain unweighted RMS: inputLoudestRmsDb / inputTypicalRmsDb
        // (p90/p50 of the 100 ms hops)."*
        //
        // The slot legs have in fact never been K-weighted (LevelTally::Weighting::Plain: "a
        // compressor's detector never hears K-weighting"), so the figures above were already dBFS
        // RMS - but they are percentiles of 400 ms MOMENTARY blocks returned as 1 dB bin centres,
        // which is why his kick read the same number twice. These are the hop percentiles: raw,
        // unquantised, and over the hops that were actually heard. rmsSeconds says how much
        // audible audio they describe, so the figure is never quoted without its window.
        bool  rmsKnown = false;
        float loudestRmsDb = -200.0f, typicalRmsDb = -200.0f;
        float rmsSeconds = 0.0f;
        // ---- AND THE OUTPUT LEG, AND BOTH CRESTS (19 Oct 2026, for B's no-profile fallback) ---
        // *"check gr on a third-party slot should also return the slot's OUTPUT level and both
        // crests over the same window: outputLoudestRmsDb / outputTypicalRmsDb, plus
        // inputCrestDb / outputCrestDb (peak minus RMS), named as measurements, no GR field."*
        // In minus out is NOT computed here and never will be: a difference between two legs is
        // the figure his 18 Oct ruling removed, and B derives whatever it derives from the profile.
        bool  outRmsKnown = false;
        float outLoudestRmsDb = -200.0f, outTypicalRmsDb = -200.0f;
        bool  inCrestKnown = false,  outCrestKnown = false;
        float inCrestDb = 0.0f,      outCrestDb = 0.0f;
    };
    std::function<SlotInputLevels (const juce::String& uid, int slot1Based)> slotInputLevels;

    // identity
    std::function<juce::String()> ownName;       // the channel's display name
    std::function<juce::String()> ownKind;       // channel | bus | mix_bus | master
    std::function<int()> ownWidth;               // the OWN rack's channel count (0 = unknown)
    /** channelRole for a target ("" = this chain), composed by ChainRole::toVar() on the side that
        knows - so the agent road and agent off send ONE shape (round (b), 20 Oct 2026). */
    std::function<juce::var (const juce::String& uid)> channelRoleVar;
    /** ---- WHAT THE TARGET HAS HEARD (Sean's 18:15 item 2, 20 Oct 2026) --------------------
        *"B saw no heard playback on own-channel agent turns, even though Sean played first."*

        And the finding was worse than the report: these two blocks are CHAT-ROAD fields
        (EchoJayAPI's body), so the AGENT road has never sent either of them - on a Link turn or an
        own-channel one. All the agent start context ever carried about playback was
        transport.heardSeconds.

        Both composed by the side that knows, for a TARGET ("" = this chain), so the agent road sends
        the same shapes the chat road does rather than a second spelling of them. Void = nothing to
        say, which both roads already express as an absent field. */
    std::function<juce::var (const juce::String& uid)> trackLevelVar;
    std::function<juce::var (const juce::String& uid)> slotLevelsVar;
    std::function<juce::String()> projectName;
    // racks
    std::function<RackRead()> ownRack;
    std::function<RackRead (const juce::String& uid)> linkRack;
    std::function<std::vector<TrackRead>()> tracks;
    // analysis
    std::function<AnalysisRead()> ownAnalysis;
    std::function<AnalysisRead (const juce::String& uid)> linkAnalysis;
    // the own rack's tallies (the loop's own readings; LevelTally snapshots)
    // levelling: the rack-level record (own rack: the loop + the stored record; a Link: its sidecar's record)
    std::function<LevellingRead()> ownLevelling;
    std::function<LevellingRead (const juce::String& uid)> linkLevelling;
    std::function<echojay::LevelTally::Snapshot()> chainInLoop;   // loop-owned input window (08c F2)
    std::function<echojay::LevelTally::Snapshot()> chainOut;      // the chain output window
    // the window: resets ONLY the loop-owned tallies (LoudnessLoop::startWindow's list), never the song's integrated reading
    std::function<void()> beginWindow;
    /** Start a new gain-reduction window on every slot that reports one (15 Oct 2026). Separate from
        beginWindow because that one is the LOUDNESS loop's list and its contents are Sean's 13:22
        ruling; the GR window is the devices' own and is reset at the same moment. */
    std::function<void()> beginGrWindow;
    std::function<bool()> transportKnown, transportPlaying;
    /** B's keep-playing contract (22 Oct 2026): the plugin a keep_playing dial is still working
        on, or empty. Bound to the processor, which is where both the names and the dials' state
        live; absent in a harness, which then answers "nothing is waiting". */
    std::function<juce::String()> keepPlayingPlugin;
    /** The landed dials' own sentences, by plugin name (his 08:43 ruling). */
    std::function<juce::StringPairArray()> dialResults;
    /** A1a (B's dial-step contracts): the three the dial frame needs. `slotName` answers for the
        rack this run targets, EMPTY for an empty, out-of-range or unreachable slot - which the
        client reads as "the slot changed", the honest reading of every one of those. `dialReport`
        is the landed dial's figures. `armDialAtSlot` arms the dial the frame carries and says why
        when it cannot; it takes the SAME road an add's dial takes. */
    std::function<juce::String (int slot1)> slotName;
    std::function<juce::var (const juce::String& plugin)> dialReport;
    std::function<bool (const juce::var& dial, int slot1, juce::String& why)> armDialAtSlot;
    /** "Only suggest plugins EchoJay can auto-dial" - his Settings toggle, READ LIVE on every
        request (his 09:49: an unmapped plugin was added with it on, because neither agent body
        carried it). Same key the chat road uses: `autoDial`. */
    std::function<bool()> autoDialOnly;
    /** The header's genre selector (e.g. "Hip-Hop"), read live. B's mix-bus loudness default depends
        on it and it arrived EMPTY on agent starts, because only the chat road sent it (B, 21 Oct). */
    std::function<juce::String()> genre;
    std::function<int()>  chainRevision;
    std::function<juce::StringArray()> inventory;   // installed plugin names; empty = unknown
    /** The installed plugins that CANNOT load in this host, each with the reason, as
        [{name, why}] (17 Oct 2026, Sean's item 3).

        Until today the inventory simply REMOVED them, which hid two things: the agent could not say
        "that one will not load here" because it could not see them, and a plugin the user NAMED went
        down the add path, which does not consult the list - so Pro-C 2 went behind an Apply and
        failed on its missing arm64 slice, and kHs Transient Shaper was proposed a second time after
        failing once in the same session. Marked, not hidden. */
    std::function<juce::var()> notLoadable;

    /** ---- THE RECENT CHAT, SO A REPLY CONTINUES THE JOB IT ANSWERS (16 Oct 2026, Sean) ---------
        The last few turns of this conversation, oldest first, as [{role, text}].

        His step 6, and he called it top priority: he asked how much gain reduction the compressor
        was doing, the run said *"Go ahead and play the loudest section so I can read the gain
        reduction"* and ENDED; he replied *"im playing it"*, and a NEW run started with no memory of
        the goal, measured LUFS instead, and answered a different question.

        A run that cannot see what was just said cannot continue it, and the start body carried
        nothing but the goal. His rule (a): "every agent run gets the recent chat (my messages and
        the agent's last reply) as context, so a reply continues the job it answers."

        This is the plugin's half - sending it. Using it is B's (FOR_B §16). Void when the host has
        no chat to offer, which reads exactly as it did before the field existed.                   */
    std::function<juce::var()> recentChat;
};

class ExecutorRead : public ToolExecutor
{
public:
    explicit ExecutorRead (Sources s);
    ~ExecutorRead() override { aliveToken_.reset(); }

    // The session's channel: "" = this instance's own rack, else a Link uid (the active chat's). Set at start.
    void setTarget (const juce::String& linkUid) { target_ = linkUid; }
    juce::String target() const { return target_; }
    juce::String targetUid() const override { return target_; }   // the base's name for it

    // ToolExecutor
    juce::var startContext() override;
    bool autoDialOnly() override { return src_.autoDialOnly && src_.autoDialOnly(); }
    juce::String genreName() override { return src_.genre ? src_.genre() : juce::String(); }
    void look  (const ToolCall& call, Done done) override;
    void doOp  (const ToolCall& call, Done done) override;
    void check (const ToolCall& call, Done done) override;
    void beginPlaybackWindow() override;

    PlaybackReading readPlayback() override;
    /** B's keep-playing contract (22 Oct 2026): the plugin a keep_playing dial is still setting, or
        empty. Const because the card asks it on every repaint (point 2's step) and the client asks
        it before every stop prompt (point 1) - it reads the racks and changes nothing. */
    juce::String keepPlayingPlugin() const override;
    juce::StringPairArray dialResults() const override;
    // A1a: the dial frame's three reads - what stands in a slot, the landed figures, and the arm.
    juce::String slotPluginName (int slot1) const override;
    juce::var dialReport (const juce::String& plugin) const override;
    bool armDialAtSlot (const juce::var& dialOnPlayback, int slot1, juce::String& why) override;
    juce::String captureCheckpoint (const juce::String& label) override;
    juce::String describeOtherChannel (const ToolCall& call) override;   // item 2: the track name, or "" for self
    juce::StringArray buildWouldRemove (const ToolCall& call) override;  // step 7.4: what a build takes OFF the rack
    void undoStep (const juce::String& undoToken, Done done) override;
    void undoToCheckpoint (const juce::String& checkpoint, Done done) override;

    // ---- the pure shapes (the guard reads these directly) ----
    static juce::var rackVar (const RackRead& r, const LevellingRead& lv = LevellingRead());
    static juce::var levellingVar (const LevellingRead& lv);       // {} when ! present
    static juce::var channelVar (const juce::String& name, const juce::String& kind, const juce::String& uid,
                                 const std::vector<TrackRead>& tracks, const juce::String& project);
    static juce::var tracksVar (const std::vector<TrackRead>& tracks);
    static juce::var analysisVar (const AnalysisRead& a);
    static juce::var levelsVar (const RackRead& r, const echojay::LevelTally::Snapshot& in, const echojay::LevelTally::Snapshot& out);
    static juce::var inventoryVar (const juce::StringArray& names);
    // ITEM 9 (13 Oct 2026): the paged, filtered inventory. Parameter names for B's tool schema:
    //   match  (string, optional)  every space-separated token must appear in the name, case-insensitive
    //   offset (int,    optional, default 0)
    //   limit  (int,    optional, default kInventoryPage, capped at kInventoryPageMax)
    // and the result carries total / offset / limit / returned / hasMore / names, with `count` keeping its
    // pre-paging meaning (== total, the whole list) and `truncated` keeping its (names are only a slice).
    /** @param notLoadable  [{name, loadable:false, why, type?}] for the WHOLE machine, or void. On an
                              ordinary page: folded in page-scoped and capped, counted in
                              `notLoadableTotal`, BEFORE the 8 KB gate (17 Oct 2026, FOR_B §25). On a
                              CENSUS page: all of it, unscoped - see `census`.
        @param census        A CENSUS PAGE (18 Oct 2026, B's FOR_A 76). The server's own
                              `look(inventory)` sweep, call ids `tc_census_<n>`, which the step
                              endpoint does not trim at 8 KB. Three differences and nothing else:
                              `limit` is honoured up to kInventoryCensusMax, the cannot-run list is
                              sent WHOLE and unscoped (page-scoping could only ever name a plugin
                              that is IN the page, and a withheld plugin is by definition not in the
                              offerable list - which is why Sean's run named none of 180), and the
                              8 KB gate is replaced by kCensusMaxBytes with the trim taken off the
                              END of `names` so `offset`/`hasMore` still describe what was sent.
                              Everything about an ordinary page is untouched. */
    static juce::var inventoryPage (const juce::StringArray& all, const juce::String& match,
                                    int offset, int limit, juce::var notLoadable = juce::var(),
                                    bool census = false);
    static constexpr int kInventoryPage    = 60;
    static constexpr int kInventoryPageMax = 200;
    // ---- THE CENSUS PAGE (18 Oct 2026, B's FOR_A 76) -----------------------------------------------
    // "what compressors do I have?" answered with the 60-name page's count because the server had to
    // walk Sean's 1,577 plugins 200 at a time: eight pages, nine plugin steps, and a model round
    // between each. 2000 is B's figure and covers every machine either of us has seen; the byte
    // ceiling is twice the 256 KB B says a census page may be, so a page of long names WITH the
    // cannot-run reasons beside them still goes whole rather than being cut in half.
    static constexpr int kInventoryCensusMax = 2000;
    static constexpr int kCensusMaxBytes     = 524288;
    /** TRUE for the server's own inventory sweep, by the call id B stamps on it. The id is the marker
        because it is the same thing the step endpoint keys its own no-trim rule on: one fact, read the
        same way on both sides, so the plugin cannot send bytes the server will refuse. */
    static bool isCensusCallId (const juce::String& id) { return id.startsWithIgnoreCase ("tc_census_"); }
    // THE 8 KB GATE: trims `names`-style arrays and long strings until the JSON fits, marking truncated:true.
    static juce::var fitUnder8K (juce::var v);
    static int jsonBytes (const juce::var& v);
    /** ---- ITEM 13 (18 Oct 2026): HOW OLD A LINK'S READING MAY BE AND STILL BE QUOTED ---------
        The Link republishes on its own cadence (the sidecar's publish tick, ~100 ms when the rack
        changes and on the meter tick), so a few seconds covers an ordinary gap between a publish and
        a tool call. Older than this is not a measurement of what is playing now, and his ruling is
        that a figure nobody can check is worse than no figure. Deliberately a named constant: the
        number is a claim about freshness and belongs where it can be argued with. */
    // 18 Oct 2026, after his 18:37 run: the Link republishes every 500 ms while it is measuring, and
    // a tool round trip adds its own second or two - so a bound of 6 s covers "the reading is from the
    // section he just played" with room for one missed publish, and still refuses a window from
    // before it. The number is a claim about freshness and belongs where it can be argued with.
    // A MEMBER, NOT A CONSTANT, for the reason EJAgentClient::stallTimeoutMs is one: a bound a guard
    // cannot set is a bound no leg can hold against both of its sides. The shipped value is asserted
    // by a leg of its own, so lowering it in a harness cannot quietly become lowering it in the
    // product (18 Oct 2026).
    juce::int64 linkMeterStaleMs = 6000;
    /** When the playback window this run waited for began (0 = none in this run). A Link reading
        stamped at or after it is of THAT section, however long the round trip then took - his
        ruling, 18 Oct, and a better instrument than the wall clock. */
    juce::int64 playbackWindowStartedMs_ = 0;
    static constexpr int kMaxResultBytes = 8192;
    static constexpr int kMaxSettingsChars = 160;
    static constexpr int kMaxBands = 6;

    struct Checkpoint
    {
        juce::String token, label, targetUid;
        juce::int64 timeMs = 0;
        RackRead rack;
        echojay::LevelTally::Snapshot out;
        AnalysisRead analysis;
        LevellingRead levelling;
    };
    const Checkpoint* checkpoint (const juce::String& token) const;

protected:
    /** Everything this machine has installed, for the client's name-vs-sentence test (item 5) and
        for ExecutorDo's same-role alternatives (D22). One accessor, both callers. */
    juce::StringArray installedPluginNames() override
    { return src_.inventory ? src_.inventory() : juce::StringArray{}; }
    /** The gate this class already owns, offered to the client for a last-moment oversize. */
    juce::var compactResult (juce::var v) override { return fitUnder8K (std::move (v)); }
    /** 8 KB for everything, and the census ceiling for the server's own inventory sweep - the page
        this executor builds for those ids is deliberately bigger than the contract's cap. */
    int maxResultBytes (const juce::String& callId) override
    { return isCensusCallId (callId) ? kCensusMaxBytes : kMaxResultBytes; }
    /** What cannot load here, as [{name, loadable:false, why}] - empty when nothing is marked. */
    juce::var notLoadableHere() const { return src_.notLoadable ? src_.notLoadable() : juce::var(); }

protected:   // ExecutorDo is a subclass and item 6(b) needs the channel resolver, the "channels that
             // exist" sentence and the read sources to route and gate a cross-channel edit. These were
             // private, which is why doOp could only ever act on its own target.
    RackRead targetRack() const;
    AnalysisRead targetAnalysis() const;
    LevellingRead targetLevelling() const;
    LevellingRead levellingFor (const juce::String& uid) const;   // "" = own
    juce::String resolveChannelArg (const ToolCall& call) const;
    // item 6(d): the channels that DO exist, appended to every unknown_channel failure
    juce::String knownChannelsSentence() const;   // "" own | uid
    /** FIX 3, 14 Oct: no longer static - it reads the TRANSPORT, to tell "nothing is playing, ask the
        user to press play" from "the transport is running and this channel is silent". */
    ToolOutcome notPlaying (const juce::String& what) const;
    // ---- ITEM 8 (Sean, 13 Oct 2026): ONE SENTENCE FOR "NOT FROM A LINK YET" --------------------------
    // Every measurement read used targetRack() and a slot number, so a GR read aimed at another track came
    // back as "slot 2 is not on the rack (1..0)" - a true statement about the wrong rack, and the sentence
    // Sean's agent then retried eighteen times. The measurement half of item 1 is deferred to 0a, so the
    // honest answer is that it cannot be read, said once, in words that close the subject.
    static ToolOutcome notFromALink (const juce::String& what);
    // True when the rack this call would MEASURE is a Link: either the call names another channel that
    // resolves to a uid, or this session is itself addressing one. Both are Links, and neither can be
    // measured until 0a lands the transport - so both get the same sentence rather than a slot error.
    bool measuringALink (const ToolCall& call) const;
    /** ---- THE RACK THIS CALL IS ABOUT (19 Oct 2026) -------------------------------------------
        The call's `channel` when it names one, this session's target when it does not - and an
        INVALID RackRead (carrying `why`) when the argument resolves to nothing, so the caller
        refuses with unknown_channel rather than answering about whatever is open.

        Written as one function because it is the third place to need the idiom: the build claim
        resolves it this way, doOp gates on it through resolveTargetWithConfirm, and check(gr) did
        NOT - it read targetRack() and answered about the open channel whatever the call said. Two
        copies agreed, which is exactly why the third got written from memory and disagreed (the
        same lesson as ITEM 1's one cross-channel gate, 14 Oct). Use `.uid` from the result to
        address anything else about the same rack: a measurement must come from the rack that
        answered, not from a uid computed again beside it. */
    RackRead rackForCall (const ToolCall& call) const;
    /** The {uid, name, kind} the payload carries so the reader can see WHICH channel answered.
        "self" for the open rack, which is the spelling rackVar has always used. */
    static juce::var channelVar (const RackRead& r);
    /** ITEM 13 (18 Oct 2026): can the target in front of us have its gain reduction measured? TRUE
        for the open rack always, and for a Link whose build publishes its meters. Freshness is NOT
        asked here: that is a question about one reading at one moment, and the start context is a
        statement about what may be PLANNED. */
    bool linkMetersAvailable() const;

    // ---- ITEM 1 (13 Oct 2026): THIS EXECUTOR OUTLIVES NOTHING ----------------------------------------
    // doOp's apply now waits for the Link's ack before it reports, so its completion runs LATER - possibly
    // after the editor that owns this executor has gone. That is the exact shape of the LinkProcessor crash
    // the gate caught on 12 Oct (four callAsync([this]...) with no liveness check), so the same idiom is
    // here before the first late callback can exist rather than after a crash report names it.
    // Reset as the destructor's FIRST statement; every deferred lambda captures the weak half.
    std::shared_ptr<bool> aliveToken_ { std::make_shared<bool> (true) };
    std::weak_ptr<bool>   alive() const { return aliveToken_; }

    Sources src_;
    juce::String target_;
    std::map<juce::String, Checkpoint> checkpoints_;
    int checkpointSeq_ = 0;
    // the playback window
    bool  windowOpen_ = false;
    float linkHeardBase_ = 0.0f;     // a Link's heardSeconds at window start (its tally is not ours to reset)
    juce::uint32 linkSeqAtOpen_ = 0;
};

// Bind the sources to the real plugin objects. Message thread. The Link half reads the registry row, the rack
// sidecar (LinkShm::readRackSidecar) and the published meter frame (EchoJayProcessor::readLinkMeterFrame /
// linkLastGoodFrame); the own half reads ChainHost, its tallies and MeterEngine. `inventory` is left for the
// editor to bind (the installed list lives with the scanner / recommendable feed, hook E2b).
Sources bindToProcessor (EchoJayProcessor& proc);

} // namespace echojay::agent
