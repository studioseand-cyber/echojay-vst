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
//  WHAT STILL NEEDS A WINDOW, AND SAYS SO. One of the eight sinks cannot be moved
//  without moving the plugin's whole build road with it: `buildChain` runs
//  loadChainFromJson / sendChainToLink, ~750 lines of the most load-bearing path
//  in the plugin, which drive the chain panel, the tab, the failed-plugin prompt
//  and the chat's own follow-ups. Moving that is its own piece of work and is
//  named as such in docs/NEXT_ROUND_QUEUE.md.
//
//  Until it moves, a build is PARKED rather than lost: the step waits, bounded,
//  for a window to attach and then takes the user's own road; if no window opens
//  it fails with that reason in plain words. That is a smaller claim than "the run
//  survives the window", and it is written down here rather than left for someone
//  to discover - everything else in a run (edits, OUT moves, reads, undo, asks,
//  approvals, the closing sentence) genuinely does survive.
// =============================================================================
#include <JuceHeader.h>
#include <memory>
#include "EJAgentClient.h"
#include "EJAgentExecutorRead.h"
#include "EJAgentRunView.h"
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

    /** How long a parked build waits for a window before it gives up and says so.
        A tunable so a guard can shorten it rather than wait the real bound out. */
    int parkedBuildWaitMs = 120'000;

    /** Lines the run produced with no window attached, for the next editor to
        render. The chat path's orphanedChatTurns_ in all but name, and for the
        same reason: a reply nobody can see is indistinguishable from no reply. */
    juce::Array<juce::var> takeParkedLines();
    int parkedLineCount() const noexcept { return parked_.size(); }

    /** The run's key and session, so a reopened window can find the row it owns. */
    juce::String runKey() const { return runKey_; }
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
    /** Whether a build is waiting for a window right now - published to the rig. */
    bool buildParked() const noexcept { return parkedBuild_ != nullptr; }

private:
    void agentChanged() override;
    void agentTranscript (const juce::String& role, const juce::String& text) override;

    /** Write the run's snapshot into the processor's chat history, creating the
        entry the first time. THE one author of that record. */
    void writeSnapshot();
    void parkLine (const juce::String& role, const juce::String& text, bool live);
    void recordAnswerOnRow (const juce::String& answer);
    EchoJayProcessor& proc_;
    std::unique_ptr<echojay::agent::ToolExecutor> executor_;
    echojay::agent::ExecutorRead* read_ = nullptr;
    std::unique_ptr<EJAgentClient> client_;
    std::function<void (const juce::String&, const juce::String&)> buildRoad_;
    juce::String runKey_, lastSession_;
    // 18 Oct 2026: which run runKey_ belongs to. The sessionId could not answer that at the moment a
    // run starts (it is empty until the server replies), so a second run adopted the first one's
    // record - see newRunCheck().
    int lastSerial_ = 0;
    /** THE HISTORY'S DEPTH WHEN THIS RUN STARTED (16 Oct 2026). "Undo all" means "put back
        everything this run did", which is a walk back to a point - and the point only exists if
        somebody wrote it down when the run began. -1 = no run has started since this object did. */
    int checkpointDepth_ = -1;
    juce::String lastCheckpoint_;   // so the depth is taken once per run, not on every change
    /** AN ALIVE FLAG, NOT AN ADDRESS (fix 6's rule, 15 Oct). Every deferred poll this object
        starts captures it, so a poll that fires after the plugin has gone returns instead of
        reading a dead object - and an address that happened to be reused could not pretend to
        be this one. */
    std::shared_ptr<std::atomic<bool>> alive_ { std::make_shared<std::atomic<bool>> (true) };
    juce::Array<juce::var> parked_;

    struct ParkedBuild
    {
        juce::String uid, chainJson;
        int slots = 0;
        juce::int64 sinceMs = 0;
        std::function<void (bool, juce::String)> cb;
    };
    std::unique_ptr<ParkedBuild> parkedBuild_;
    void servicePark();
};
