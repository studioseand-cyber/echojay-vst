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

struct Sources
{
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
    std::function<echojay::LevelTally::Snapshot()> chainInLoop;   // loop-owned input window (08c F2)
    std::function<echojay::LevelTally::Snapshot()> chainOut;      // the chain output window
    // the window: resets ONLY the loop-owned tallies (LoudnessLoop::startWindow's list), never the song's integrated reading
    std::function<void()> beginWindow;
    std::function<bool()> transportKnown, transportPlaying;
    std::function<int()>  chainRevision;
    std::function<juce::StringArray()> inventory;   // installed plugin names; empty = unknown
};

class ExecutorRead : public ToolExecutor
{
public:
    explicit ExecutorRead (Sources s);

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
    void undoStep (const juce::String& undoToken, Done done) override;
    void undoToCheckpoint (const juce::String& checkpoint, Done done) override;

    // ---- the pure shapes (the guard reads these directly) ----
    static juce::var rackVar (const RackRead& r);
    static juce::var channelVar (const juce::String& name, const juce::String& kind, const juce::String& uid,
                                 const std::vector<TrackRead>& tracks, const juce::String& project);
    static juce::var tracksVar (const std::vector<TrackRead>& tracks);
    static juce::var analysisVar (const AnalysisRead& a);
    static juce::var levelsVar (const RackRead& r, const echojay::LevelTally::Snapshot& in, const echojay::LevelTally::Snapshot& out);
    static juce::var inventoryVar (const juce::StringArray& names);
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
    };
    const Checkpoint* checkpoint (const juce::String& token) const;

private:
    RackRead targetRack() const;
    AnalysisRead targetAnalysis() const;
    juce::String resolveChannelArg (const ToolCall& call) const;   // "" own | uid
    static ToolOutcome notPlaying (const juce::String& what);

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
