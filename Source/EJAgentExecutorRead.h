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
};

struct TrackRead
{
    juce::String uid, name;
    bool  connected = false, audioFlowing = false, active = true, fresh = true;
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
};

struct Sources
{
    /** FIX 1(b), 14 Oct: ONE SLOT PARAMETER, BY NAME, FOR A READ-BACK. `uid` empty = the open rack, where
        the value comes from the device itself (numeric, exact). A Link's values come from its own sidecar,
        which is the only thing readable across the process boundary. False = not readable, which the caller
        reports as "could not confirm" and never as success. */
    std::function<bool (const juce::String& uid, int slot1Based, const juce::String& paramId, double& out)> slotParam;

    /** ---- IS THIS SLOT'S DIAL STILL RUNNING? (16 Oct 2026, the joint live pass) --------------------
        "" uid = the own rack, else a Link's. TRUE while the slot's dial is PENDING: the map has been
        asked for and has not arrived, or the write has not been read back yet.

        The read-back needs it to tell two different facts apart. A third-party add on a Link read back
        `Threshold is 0.00 (asked -18.00)` in two of five live attempts and said "the settings did NOT
        take" - a positive claim that the write failed, made four seconds into a round trip that runs
        V2 -> server -> V2 -> Link -> dial -> sidecar. Unset reads false, which is the settled case and
        the behaviour this build had before the field existed. */
    std::function<bool (const juce::String& uid, int slot1Based)> slotDialPending;

    // identity
    std::function<juce::String()> ownName;       // the channel's display name
    std::function<juce::String()> ownKind;       // channel | bus | mix_bus | master
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

    // ToolExecutor
    juce::var startContext() override;
    void look  (const ToolCall& call, Done done) override;
    void doOp  (const ToolCall& call, Done done) override;
    void check (const ToolCall& call, Done done) override;
    void beginPlaybackWindow() override;
    PlaybackReading readPlayback() override;
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
    static juce::var inventoryPage (const juce::StringArray& all, const juce::String& match,
                                    int offset, int limit);
    static constexpr int kInventoryPage    = 60;
    static constexpr int kInventoryPageMax = 200;
    // THE 8 KB GATE: trims `names`-style arrays and long strings until the JSON fits, marking truncated:true.
    static juce::var fitUnder8K (juce::var v);
    static int jsonBytes (const juce::var& v);
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
