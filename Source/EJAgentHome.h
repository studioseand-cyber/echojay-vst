#pragma once
// =============================================================================
//  EJAgentHome — ITEM 0b (Sean, 14 Oct 2026): THE RUN SURVIVES THE WINDOW.
//
//  THE PROBLEM, EXACTLY. The agent's client, its executor and its sinks were all
//  owned by the EDITOR. Close the window mid-run and the client was destroyed:
//  the socket went, the steps went, the undo tokens went, and the run that the
//  user had started simply stopped existing - with no error, because nothing was
//  left to report one. Sean's check 9 is this bug's cousin on the chat path, and
//  item 5's fix built the pattern that answers it: the PROCESSOR keeps what is in
//  flight, and the next editor ADOPTS it.
//
//  So the agent lives here, on the processor, and the editor becomes a view:
//
//      EJAgentClient      here      (the session, the steps, the SSE socket)
//      ExecutorDo         here      (every sink reaches the processor or the API)
//      Sources            here      (bindToProcessor already needed no editor)
//      the run's RECORD   here      (the snapshot on processorRef.chatHistory)
//      the run's ROW      editor    (it is the view, and belongs there)
//
//  ONE AUTHOR FOR THE SNAPSHOT. This class writes the run's RunView into the
//  processor's chat history on every change, whether a window is attached or not.
//  The editor mirrors that entry into its own transcript and paints it. It does
//  not compute a second copy - a run described in two places is the fault item 0
//  was built to remove, and with a window closing in the middle of it the two
//  copies would differ by exactly the part that matters.
//
//  THE BUILD, AND WHAT A CLOSED WINDOW DOES TO IT (Sean, 22 Oct 2026 - his B3 ruling):
//
//      "If the user has already pressed Go, a run with no window open finishes its
//       build and applies it. The card, its chips and Undo all are there when a
//       window reopens. If the run is waiting on a chip or a question, it keeps
//       waiting."
//
//  Until that ruling a windowless build PARKED: it waited two minutes for a window
//  and then said it had built nothing. Honest about itself, and the wrong behaviour -
//  the user had already approved it, and the one step that could not survive a closed
//  window was the step this whole round exists for.
//
//  IT APPLIES NOW, AND NOT BY A NEW ROAD. The editor's loadChainFromJson and
//  sendChainToLinkOverTheWire stay the window's (they drive the chain panel, the tab,
//  the failed-plugin prompt and the chat's own follow-ups, and re-implementing them
//  here is this file's own named fault: "two build paths that looked identical in the
//  log is what let a whole class of failure sit undiagnosed"). With no window the
//  build goes down the road that is ALREADY not the editor's - the one every agent
//  `add` takes: applyChainOps, which is ChainHost::applyChainEdits for the open rack
//  and writeChainEditCommand + awaitChainAck for a Link. The SAME std::function, held
//  as applyOps_, not a second copy of its body. That is the move his 19 Oct ruling
//  made for the Build button on a Link ("make Build use the same path agent-mode adds
//  use, so there's one road that works"), arriving where it was always going.
//
//  A WAITING RUN STILL WAITS, and nothing here changes that: a build only happens
//  after Go, so a run holding an ask reaches none of this. The rig asserts both
//  halves (B3 applies with no window, B3b proves a waiting run applies nothing).
// =============================================================================
#include <JuceHeader.h>
#include <memory>
#include "EJAgentClient.h"
#include "EJAgentExecutorRead.h"
#include "EJAgentRunView.h"
#include "LoudnessLoop.h"   // his 17:3x item 3: DialOnOp, one dial per slot in a batch
#include "EJStateRoot.h"

class EchoJayProcessor;

/** AGENT MODE's switch, as a free function. It was a static member of
    EchoJayEditor, which put the one gate every mutating op consults inside the
    class that is allowed to disappear mid-run. The file and its meaning are
    unchanged - existence = on, the text = Ask or Auto - so ship_2026-10-09a,
    every guard and the Settings toggle all read exactly what they did. */
namespace echojay::agentmode
{
    inline juce::File flagFile()
    {
        return echojay::userAppData().getChildFile ("EchoJay").getChildFile ("agent_mode");
    }
    inline bool on() { return flagFile().existsAsFile(); }
    /** "off" | "ask" | "auto", read live from the file's text. Anything else is Ask. */
    inline juce::String word()
    {
        if (! flagFile().existsAsFile()) return "off";
        const auto t = flagFile().loadFileAsString().trim().toLowerCase();
        return t == "auto" ? "auto" : "ask";      // anything else (including today's "on") reads as Ask
    }
}

class EJAgentHome : private EJAgentClient::Listener
{
public:
    explicit EJAgentHome (EchoJayProcessor& proc);
    ~EJAgentHome() override;

    EJAgentClient& client() noexcept { return *client_; }
    echojay::agent::ExecutorRead* readExecutor() noexcept { return read_; }

    /** THE BUILD ROAD, registered by an editor and dropped when it goes. Set to
        {} means "no window": a build parks. One road, not two - the editor hands
        over the SAME function a user's Build press runs. */
    void setBuildRoad (std::function<void (const juce::String& uid, const juce::String& chainJson)> road);
    bool hasBuildRoad() const noexcept { return (bool) buildRoad_; }

    /** The sink ExecutorDo is given. Public because the one caller is the sink
        built in this file's .cpp, and because the rig asserts on its behaviour. */
    void buildChain (const juce::String& uid, const juce::var& chainArray,
                     std::function<void (bool, juce::String)> cb);

    /** Lines the run produced with no window attached, for the next editor to
        render. The chat path's orphanedChatTurns_ in all but name, and for the
        same reason: a reply nobody can see is indistinguishable from no reply. */
    juce::Array<juce::var> takeParkedLines();
    int parkedLineCount() const noexcept { return parked_.size(); }

    /** The run's key and session, so a reopened window can find the row it owns. */
    juce::String runKey() const { return runKey_; }
    /** WHICH CHAT THIS RUN BELONGS TO (his Test 5 fault 1, 29 Oct 2026). The run's own writer has
        always used it; the editor's transcript bridge had no way to ask, so when the row was not in
        the chat ON SCREEN it fell back to writing there - and the Aitch Link's "Stopped." landed at
        the top of the Mix Bus chat. A message belongs to its run's chat, never to the view. */
    juce::String runChatId() const { return runChatId_; }
    /** The rack this run is working on ("" = the open rack), and whether it still needs it. The
        V2's borrow tick asks both: his 08:44 ruling is that opening a Link's window must not take
        the rack from a live run, and that closing it hands control straight back. */
    juce::String runTargetUid() const;
    /** Release the run's rack once neither a live run nor an Undo all needs it. */
    void forgetRunRackIfDone();
    bool runNeedsItsRack() const;
    /** Is a run WORKING on its rack right now - as opposed to finished with its Undo all still
        live? The shield and the panel's lock ask this one; the engage asks runNeedsItsRack.
        See echojay::runBlocksUsersKnobs for the ruling behind the split (his 17:25, 25 Oct). */
    bool runIsWorking() const;
    /** THE KEY, MINTED HERE IF IT DOES NOT EXIST YET (Sean's #11, 16 Oct 2026). This object is the
        one author of the run's RECORD, and the editor's row is a mirror of it - but a run started in
        a LIVE window had the editor mint a key of its own first and push a second chat-history entry,
        so one run left two records: the row, and a plain bubble carrying the closing sentence. "The
        closing shows twice: as a separate chat bubble AND inside the run message." One key, from
        here, whoever asks first. */
    juce::String ensureRunKey();
    /** TRUE once per run, for whichever of the two minting sites asks first. The run SERIAL, never the
        sessionId - which is empty in exactly the window where a row and a record are created. */
    bool newRunSinceLastKey();
    juce::String sessionId() const { return client_ != nullptr ? client_->sessionId() : juce::String(); }
    /** WHICH ROAD THE LAST BUILD TOOK: "window" or "processor" ({} before any build in this
        session). Published to the rig as agentLastBuildRoad. His 22 Oct ruling is about exactly
        this fact, so the rig reads it from the BUILD rather than keeping a copy of its own - the
        thing that cost five rig rows on 20 Oct ("a harness copy of a product switch"). */
    juce::String lastBuildRoad() const { return lastBuildRoad_; }
    /** ...AND WHICH RUN THAT ROAD BELONGS TO (22 Oct 2026). A published fact with no run attached
        is sticky: the rig's B4 waited for "a road is named" and was answered instantly by the
        PREVIOUS run's "processor", then called the product wrong for it. One field cannot say
        which run it describes; two can, and then a row can set its own target ("a row sets its
        own target", 18 Oct). */
    juce::String lastBuildRunKey() const { return lastBuildRunKey_; }

    /** A1a (28 Oct 2026): THE ARM, FROM THE SLOT ON - the one road an add's dial and B's dial frame
        share. Static because the dial frame reaches it through the executor's sinks, which are bound
        to the processor and know nothing about an instance of this class. False with the reason in
        `whyOut`, which goes to the MODEL: a dial frame that cannot arm answers with it. */
    static bool armDialAtSlotOnOwnRack (EchoJayProcessor& proc, const juce::var& dial,
                                        int slot0, juce::String& whyOut);

private:
    void agentChanged() override;
    void agentTranscript (const juce::String& role, const juce::String& text) override;

    /** Write the run's snapshot into the processor's chat history, creating the
        entry the first time. THE one author of that record. */
    void writeSnapshot();
    void parkLine (const juce::String& role, const juce::String& text, bool live);
    /** ---- B'S dial_on_playback ON THE V2'S OWN RACK (his 11:5x item 1, 21 Oct 2026) ----------
        The Link reads this field in its chain-cmd apply; the open rack's ops come through the
        in-process applier, which read it nowhere - so his mix-bus limiter went in with an open-loop
        input gain and nothing ever measured the result (-5.7 LUFS against a target of -8).

        `addedNames` is what the batch added, so the slot is resolved BY IDENTITY after the apply
        (LoudnessLoop::slotTheBatchAdded), never by an index. Two jobs arrive on one field and are
        told apart by which key is present: `target_lufs` is a limiter's loudness target and arms the
        loudness loop at that limiter's INPUT; `target_gr_db` is a compressor's gain-reduction target
        and becomes a calibration block, the same road agent-off already takes. */
    void armDialOnPlaybackOnOwnRack (const juce::var& dial, const juce::StringArray& addedNames);
    /** ---- EVERY DIAL IN ONE BATCH (his 17:3x item 3, 21 Oct 2026) ---------------------------
        *"B will send dial_on_playback on every compressor after the first in a build ... Confirm
        that works for several slots in one build (not just the last), each dialled independently."*

        One call per batch, because the GR dials have to go to calibStartMany TOGETHER: calibStart
        stores one loop per rack uid, so calling it twice for the same rack overwrites the first -
        which would have dialled the last compressor and silently dropped the rest. calibStartMany
        is the road built for "ONE HOLD PER COMPRESSOR" (30 Sep) and this is its caller. */
    void armDialsOnPlaybackOnOwnRack (const std::vector<LoudnessLoop::DialOnOp>& dials,
                                      const juce::StringArray& addedNames);
    void recordAnswerOnRow (const juce::String& answer);
    EchoJayProcessor& proc_;
    std::unique_ptr<echojay::agent::ToolExecutor> executor_;
    echojay::agent::ExecutorRead* read_ = nullptr;
    std::unique_ptr<EJAgentClient> client_;
    std::function<void (const juce::String&, const juce::String&)> buildRoad_;
    juce::String runKey_, lastSession_;
    /** ---- WHERE THIS RUN'S ROW LIVES, HELD BY THE HOME ITSELF (his test 2, 08:28, 23 Oct) -----
        The snapshot used to be written only by walking proc_.chatHistory for a row whose key
        matched - and a chat or Link SWITCH clears that list (loadChatFromWorkspace, "a switch never
        carries one chat's messages into another"). So from his first switch onwards the loop found
        nothing and the run's card stopped being written anywhere, which is why the Vocal card sat
        at "Thinking..." with no steps while its run was perfectly alive. These two are taken when
        the row is minted and are the home's own answer to "which row is mine", so a list the window
        empties can no longer silence the record. */
    juce::String runChatId_, runEntryId_;
    // 18 Oct 2026: which run runKey_ belongs to. The sessionId could not answer that at the moment a
    // run starts (it is empty until the server replies), so a second run adopted the first one's
    // record - see newRunCheck().
    int lastSerial_ = 0;
    /** THE HISTORY'S DEPTH WHEN THIS RUN STARTED (16 Oct 2026). "Undo all" means "put back
        everything this run did", which is a walk back to a point - and the point only exists if
        somebody wrote it down when the run began. -1 = no run has started since this object did. */
    int checkpointDepth_ = -1;
    juce::String lastCheckpoint_;   // so the depth is taken once per run, not on every change
    // ---- THE RACK THIS RUN IS ON, taken when it starts (his 21:22 fault, 28 Oct 2026) --------
    // NOT the executor's live target: syncAgentTarget() moves that whenever he changes what he is
    // LOOKING AT, so a run parked on a question claimed to be "working on" whatever rack he
    // happened to open. Held for the run's life, cleared when no run needs the rack any more.
    juce::String runRackUid_;
    // HAS it been taken? Empty is a real answer (the own rack), so emptiness cannot also mean
    // "not taken yet" - that conflation is C's point 37 and it reinstated the whole fault.
    bool         runRackTaken_ = false;
    // WHICH run took it (EJAgentClient::runSerial). Without this, a second run inherited the
    // first one's rack for as long as the first one's Undo all was live - C's point 42.
    int          runRackSerial_ = -1;
    /** AN ALIVE FLAG, NOT AN ADDRESS (fix 6's rule, 15 Oct). Every deferred poll this object
        starts captures it, so a poll that fires after the plugin has gone returns instead of
        reading a dead object - and an address that happened to be reused could not pretend to
        be this one. */
    std::shared_ptr<std::atomic<bool>> alive_ { std::make_shared<std::atomic<bool>> (true) };
    juce::Array<juce::var> parked_;

    /** THE OPS ROAD, kept because a windowless BUILD takes it (his 22 Oct ruling above). It is
        the same std::function the executor's applyChainOps sink is given, assigned in the
        constructor beside it - one implementation of "apply these ops to that rack", so a build
        with no window cannot drift from an add with no window. */
    std::function<void (const juce::String&, const juce::var&,
                        std::function<void (bool, juce::String, juce::String)>)> applyOps_;
    juce::String lastBuildRoad_, lastBuildRunKey_;
};

namespace echojay
{
// ---- ITEM 2 (Sean, 19 Oct 2026, his 15:36 run): AN AGENT ADD RE-LEVELS LIKE A BUILD ------------
//
// *"Agent-off Build level-matches on the slot card's OUT trim on playback. Agent builds should get
// the same silent slot-OUT level match after a plugin is added (here the Shadow Hills dropped the
// vocal 2.5 dB and nothing compensated). If B also sets the OUT trim from a check, B's explicit
// value wins; don't fight it."*
//
// The mechanism was already there and the agent's road simply never asked for it: `intent:"build"`
// is what makes a Link arm its post-build level match (writeChainEditCommand's own documentation),
// the agent-off Build passes it, and the agent's applyChainOps sink passed nothing - so the one
// kind of change that alters a rack's loudness with no human at the controls was the one kind that
// never re-levelled.
//
// WHICH OPS COUNT. Only an op that puts a plugin in: `add` and `replace`. An ordinary edit must
// never re-level a rack (same documentation), so a set, a bypass, a move or a wet change arms
// nothing - a level match is not a thing to do to a rack because a knob moved.
//
// AND B'S VALUE WINS. A batch that carries an explicit `slot_out` has already been told what the
// trim should be, by B reading a check; arming the loop on top of it would be two authors writing
// one number, and the loop would overwrite the value it was told. So a slot_out in the batch
// withdraws the arm. (The LATER command's case - a slot_out arriving while a match armed by an
// earlier add is still waiting - is LoudnessLoop::standDownQuietly, called from LinkProcessor.)
//
// In the header rather than file-static so the rule is reachable by name from a guard: the three
// answers it gives are the whole of item 2's contract on the sending side.
inline juce::String agentIntentForOps (const juce::var& ops, juce::String* whyNot = nullptr)
{
    const auto* arr = ops.getArray();
    if (arr == nullptr) return {};
    bool putsAPluginIn = false, carriesAnExplicitOutTrim = false;
    for (const auto& o : *arr)
        if (auto* oo = o.getDynamicObject())
        {
            const auto name = oo->getProperty ("op").toString().trim().toLowerCase();
            if (name == "add" || name == "replace") putsAPluginIn = true;
            if (name == "slot_out")                 carriesAnExplicitOutTrim = true;
        }
    if (! putsAPluginIn)
    {
        if (whyNot != nullptr) *whyNot = "no op in this batch puts a plugin in";
        return {};
    }
    if (carriesAnExplicitOutTrim)
    {
        if (whyNot != nullptr)
            *whyNot = "the batch names a slot OUT trim itself, and the explicit value is the one "
                      "that was asked for";
        return {};
    }
    return "build";
}
}   // namespace echojay
