# Agent mode, plugin side — Session A2 handoff (9 October 2026, branch `feat/agent-ui`, worktree `~/echojay-vst-a2`)

**Status: WRITTEN, NOT COMPILED.** A2 never compiles, gates, installs or runs Logic (EJ Map was scanning; A owns
every build on this Mac). Every file below was written against the headers it includes, read today, but the first
compiler to see them is A's. Expect the usual first-compile fixes; the design will not change under them.

Read alongside: `docs/LINK_REMOTE_CONTROL_PLAN.md`, `HANDOFF_COMP_PROFILES_PLUGIN.md` (08c), and
`~/echojay-saas/docs/AGENT_MODE_PLAN.md`. The brief's `docs/CONTRACT_AGENT_TOOLS.md` **does not exist** in the saas
repo (any branch, 9 Oct 15:00); the wire shapes here follow the plan's section 1.3 examples and are written to be
corrected in ONE file (`EJAgentProtocol.h`) when B's contract lands.

---

## 1. Files (all NEW; nothing existing was edited)

| file | what | depends on |
|---|---|---|
| `Source/EJAgentFraming.h` | SSE splitter that KEEPS `event:` names. `EJStreamFraming` drops them by contract (chat-stream never sends named events); the agent frames are typed by them. JUCE-free, standalone-testable. | nothing |
| `Source/EJAgentProtocol.h` | the wire vocabulary as pure functions: `parseFrame(eventName, json)` (type from the event line FIRST, from a `"type"` property SECOND), `ToolCall`, `ToolResult`, `buildStepBody`, `buildStartBody`, `describeCall`, `isPlaybackWait`, `talkAskOf`. Header-only, `juce_core` only. Default approval when a frame carries none: `do` asks, everything else free (the safe direction). | juce_core |
| `Source/EJAgentTools.h` | **the executor interface A implements** (`echojay::agent::ToolExecutor`) + `StubExecutor` (answers `not_in_phase` to everything, counts no playback, has no checkpoint: honest, so the loop and cards are exercisable before A's lands). | EJAgentProtocol.h |
| `Source/EJAgentClient.h/.cpp` | the loop driver: `/api/agent/start` + `/step` as SSE POSTs (one-byte reads, cancel handle, `net::Worker` census entry, callAsync behind alive + generation), rounds, strict wire-order execution, the plan decision, `talk(ask)`, `wait_for_playback` (client-run, never hangs), Stop, Undo per step / Undo all, Retry, a 90 s stall watchdog. Renders nothing. | EJAgentFraming, EJAgentProtocol, EJAgentTools, `EJNetCensus.h` |
| `Source/EJAgentPanelLayout.h` | the panel's geometry as pure functions (`wrappedTextHeight`, `layoutRows`, `layoutChips`, `checkRows`). One author for paint and buttons; what the guard asserts. | JuceHeader |
| `Source/EJAgentPanel.h/.cpp` | the card: header (status dot, status line, **Stop**), scrolling body (goal, streamed talk, notice, **the checklist** with glyphs + the server's summary + landed line/error + per-row **Approved/Skipped** toggles while the plan waits and **Undo** once allowed, the **ask** question + choice pills, the **playback** ring + sentence), footer (**Approve all / Apply / Decline**, **Got it, you can stop**, **Undo all / Retry / Close**). EchoJay palette (`EchoJayLookAndFeel::Colours`), the chat's 26 px pills, `kFieldCorner`. | EJAgentClient, EJAgentPanelLayout, `EchoJayLookAndFeel.h` |
| `Tests/test_agent_framing.cpp` | standalone: `c++ -std=c++17 Tests/test_agent_framing.cpp -o /tmp/ej_agent_framing && /tmp/ej_agent_framing` | nothing |
| `tools/agent_client_guard/harness.cpp` | the legs (section 5), in-process, no network: a recorder replaces the socket through the ONE named seam (`EJAgentClientTestAccess`, declared friend), a fake executor replaces A's. | the V2 archive |

**CMake lines for A** (`CMakeLists.txt`, `target_sources(EchoJay PRIVATE ...)`, anywhere in the list):
```
    # --- agent mode, plugin side (Session A2, feat/agent-ui) ---
    Source/EJAgentFraming.h
    Source/EJAgentProtocol.h
    Source/EJAgentTools.h
    Source/EJAgentClient.cpp
    Source/EJAgentClient.h
    Source/EJAgentPanelLayout.h
    Source/EJAgentPanel.cpp
    Source/EJAgentPanel.h
```
and in `tools/tests/CMakeLists.txt`, with the fast guards:
```
ej_add_guard(agent_client_guard       SOURCE tools/agent_client_guard/harness.cpp              LABEL fast)
```
(`Source/*.cpp` is an explicit list in four targets; the Link target does not need these.)

---

## 2. The hooks A adds (the smallest set I could find; each is a few lines)

**T1 — `EchoJayAPI` (EchoJayAPI.h, public):** the transport descriptor. `authToken`, `transportEndpoint`,
`transportHeaders` are private; `getEndpoint()` is already public.
```cpp
    // Agent mode (A2): what EJAgentClient needs to open its own SSE POSTs, read at start time.
    struct AgentTransport { juce::String baseUrl, authToken, extraHeaders, appVersion; };
    AgentTransport agentTransport() const
    { return { transportEndpoint (apiEndpoint, "/api/agent/start"), authToken, transportHeaders(), JucePlugin_VersionString }; }
```
(if `transportEndpoint`'s second argument is per-path, pass `"/api/agent/start"`; the client appends the path itself.)

**E1 — members (PluginEditor.h):**
```cpp
    #include "EJAgentClient.h"
    #include "EJAgentPanel.h"
    ...
    std::unique_ptr<echojay::agent::ToolExecutor> agentExecutor_;   // A's class (section 3); StubExecutor until then
    std::unique_ptr<EJAgentClient> agentClient_;
    std::unique_ptr<EJAgentPanel>  agentPanel_;
```

**E2 — construction (PluginEditor.cpp ctor, after `api` and the chat components exist):**
```cpp
    agentExecutor_ = std::make_unique<echojay::agent::StubExecutor>();   // -> A's executor
    agentClient_ = std::make_unique<EJAgentClient> (
        [this] { const auto t = api.agentTransport(); return EJAgentClient::Transport { t.baseUrl, t.authToken, t.extraHeaders, t.appVersion }; },
        *agentExecutor_,
        [] (const juce::String& line) { EchoJay_NSLog (line.toRawUTF8()); });
    agentPanel_ = std::make_unique<EJAgentPanel> (*agentClient_);
    addChildComponent (*agentPanel_);
    agentPanel_->onLayoutNeeded = [this] { resized(); };          // the card's height or visibility changed
    agentPanel_->onTranscript  = [this] (const juce::String& role, const juce::String& text)
    {   // E4: lines for the chat history, through the SAME two helpers the loudness loop uses
        if (role == "user") appendLocalUserBubble (text); else appendLocalResultBubble (text);
    };
```

**E3 — docking (the ask-shelf site in the chat layout, PluginEditor.cpp ~20030-20070):** dock the panel exactly as
the brief card is docked, within the chat column, and take its height off the transcript. Where the brief-card
branch computes `cardH` and calls `askShelfBounds`:
```cpp
    if (agentPanel_ && agentPanel_->shouldShow())
    {
        const int maxH = juce::jmax (120, (int) (chatScroll.getHeight() * 0.6f));
        const int h = agentPanel_->preferredHeight (chatBoxRect_.getWidth(), maxH);
        const auto r = askShelfBounds (chatBoxRect_, chatScroll.getBounds(), h);   // Round C: never past the chat column
        agentPanel_->setBounds (r);
        agentPanel_->setVisible (true);
        agentPanel_->toFront (false);
        // reserve: whatever the ask shelf does to chatScroll's bottom (askShelfRect_ / chatScrollBottom), do the same with r.getHeight()
    }
    else if (agentPanel_) agentPanel_->setVisible (false);
```
The one thing I could not settle from a read: whether the ask shelf and the agent card should stack or exclude
each other. Exclude (the agent card wins while a session exists) is my recommendation: an agent `talk(ask)` IS the
ask, rendered inside the card, so a shelf underneath would be a second question. Also keep the `compact` branch
(420x500) in mind: `preferredHeight` is width-aware and the body scrolls under `maxH`, so the same call works there.

**E5 — the send path (sendChatMessage, at the top, after the empty-message guard):**
```cpp
    if (agentClient_ && agentClient_->interceptTyped (msg)) return;      // a pending ask takes it; a running agent refuses it WITH a line
    if (agentModeOn() && agentClient_ && ! agentClient_->isActive())
    {
        agentClient_->start (msg, currentChatId);                        // the goal; the panel appears through onLayoutNeeded
        return;
    }
```
with the dev switch the plan names (section 7, "08b sends it only when its own dev flag is on"):
```cpp
    static bool agentModeOn()
    { return ChainHost::devModeActive() && echojay::userAppData().getChildFile ("EchoJay").getChildFile ("agent_mode").existsAsFile(); }
```
Off = byte-identical to today. The client puts `agentMode: true` on its own start body; nothing is added to the chat
body (B's `AGENT_MODE=1` is the other half of the switch).

**E6 — teardown (the editor destructor, where `chatStreamHandle` is cancelled):**
```cpp
    if (agentClient_) agentClient_->stop();   // cancels the socket; the destructor's alive flag covers the rest
```
Member order: `agentPanel_` must be destroyed before `agentClient_` (declare it after, or reset it first) — the
panel removes itself as a listener in its destructor.

**E7 — the header Undo button (optional, rackUndoRedo):** nothing to add. A's executor records each landed `do` as
a CHAIN entry in `processorRef.undoHistory()` (LINK_REMOTE_CONTROL_PLAN.md section 9), so the existing Undo
button walks the agent's steps too; the panel's per-row Undo and Undo all go through the executor's tokens to the
same stack.

---

## 3. The executor contract (what A's class implements; `Source/EJAgentTools.h`)

```cpp
class ToolExecutor {
  juce::var startContext();                                   // channel {role,name,uid}, rack one line per slot, inventory shortlist -> /start "context"
  void look  (const ToolCall&, Done);                         // rack | channel | analysis | inventory | chains  -> COMPACT var (~250 tokens max)
  void doOp  (const ToolCall&, Done);                         // build|add|remove|replace|move|set|set_wet|bypass|set_level|set_pre_gain
  void check (const ToolCall&, Done);                         // level | gr | spectrum | true_peak | balance (NOT playback - the client runs that)
  void beginPlaybackWindow();                                 // reset the LOOP-OWNED tally (chainInLoopTally_ precedent), never the song's integrated reading
  PlaybackReading readPlayback();                             // {transportKnown, playing, heardAboveSeconds, integratedLufs, loudestShortTermLufs, truePeakDbtp}
  juce::String captureCheckpoint (const juce::String& label); // "" = unavailable; the token "undo all" sends back
  void undoStep (const juce::String& undoToken, Done);
  void undoToCheckpoint (const juce::String& checkpoint, Done);
};
```
- `Done(ToolOutcome)`: `ok` + compact `result`, or `errorCode` + `errorMessage` (B's validators' codes:
  `unknown_control`, `range_not_allowed`, `not_in_phase`...). A landed `do` sets `undoToken` and, if it likes,
  `landedLine` (the row's second line).
- Every call is message-thread in, message-thread out (sync or async). A completion after `stop()` is ignored by
  the client (generation check), so the executor need not care.
- Phase 1 (plan section 7): `doOp` returns `not_in_phase`; `look`/`check` read the rack model and the tallies.
  Phase 2: `doOp` goes through the transport commands with acks as the results. Hook into `writeChainEditCommand`
  / `applyChainEdits` is A's.
- `PlaybackReading.transportKnown`/`playing` are `processorRef.isTransportKnown()`/`isTransportPlaying()` (the
  LoudnessLoop wiring, PluginProcessor.cpp:438). `heardAboveSeconds` is `LevelTally::Snapshot::heardAboveSeconds`
  of the window tally.

---

## 4. Behaviour, as built (what the UI does and when)

- **Start**: the typed goal becomes the user's turn (E4), the checkpoint is captured, `/start` goes out with the
  executor's context. No token -> "Sign in to use the agent." without a request.
- **A round**: `round` resets the round's talk; `delta`s stream into the card's text; each `tool_call` is a row at
  once (pending ring, or an amber ring + Approved toggle when `ask_first`); `await` starts execution in STRICT WIRE
  ORDER. A free `look`/`check`/`talk` runs at once; an `ask_first` row blocks on the plan card; a failed `do` marks
  the round's later `do`s `not_run` (looks still run). When every awaited id is answered, ONE `/step` is posted and
  the round's talk goes to the transcript once.
- **Plan card** = the checklist's ask_first rows with Approved/Skipped toggles + footer Approve all / Apply /
  Decline. Declined lines return `{ok:false, error:{code:"declined"}}`.
- **talk(ask)**: the question + choice pills in the card; a tap or a typed message answers `{choice, typed}`.
- **wait_for_playback**: "Play the chorus or the loudest section." + a ring that fills toward `min_seconds`;
  "Listening... n of m s"; at `min_seconds`: "Got it, you can stop." and the reading goes back; the user's **Got it**
  returns what was heard (`short:true` when under); a known-stopped transport after >= 1 s heard ends it early;
  **60 s timeout** returns `{played:false, waited}` and says "No playback heard in 60 s - carrying on without a
  reading." It cannot hang: every exit is on the client's own timer.
- **Stop**: generation bump + socket cancel; every unfinished row reads stopped; nothing is posted; a late executor
  completion or frame is ignored; "Stopped." in the transcript; the card stays so Undo is reachable.
- **Undo**: per row (its token) when the loop is not mid-round; Undo all -> the checkpoint; rows read "undone".
- **done** -> Done + summary in the transcript; **error** -> Failed (+ Retry when retryable, re-posting the same
  body); EOF with no terminal frame -> Failed; 90 s without a frame on an open stream -> Failed, retryable.
- **A typed message under a running agent** is refused with a line in the card (08c item C's rule: a message that
  vanishes is indistinguishable from a dropped send).
- **Sizes**: every text row's height is MEASURED (`wrappedTextHeight`), buttons never share x with text, chips
  wrap, and the body scrolls when the editor gives it less room than its content. Asserted from 380 to 1780 px.

---

## 5. Legs (tools/agent_client_guard, label fast) — written, not run

| leg | what is RED without the behaviour |
|---|---|
| P1-P9 | event-line and type-in-JSON frames parse alike; a bare `do` asks; the step body shape; both playback shapes; malformed frames skipped |
| N1-N5 | `nextRunnable`: strict wire order, -2 on an undecided ask_first, nothing while a step runs |
| R1a-n | start body; checkpoint; goal as the user's turn; deltas; rows; free look runs, check behind the plan waits; ONE step, every id, await order; talk once; undo token on the landed do; log lines |
| R2 | Decline -> `declined`, nothing ran |
| R3 | per-line skip -> declined; approved line runs; order kept |
| R4 | failed do -> later do `not_run`, look still runs; rows carry the reasons |
| A1-A5 | ask card; typed answer taken; `{choice, typed}` both ways; transcript |
| W1-W4 | timeout returns `played:false`; min reached returns the reading + "Got it, you can stop."; early stop `short`; stopped transport ends it |
| S1-S7 | Stop at once; rows Stopped; no post; late completion + stale frame ignored; "Stopped."; Undo reachable |
| U1-U3 | undo per step sends its token; undo all sends the checkpoint; notice |
| E1-E8 | error frame; Retry byte-identical; done + summary; dismiss; EOF -> Failed; no token -> Sign in |
| T1-T2 | typed under a running agent refused with a line; passes through otherwise |
| L0-L8 | the panel at 6 widths x 4 height caps in 4 scenes passes `checkLayout`; chips wrap at 380; a short cap scrolls, never cuts; re-dock asked for; hidden after dismiss |

`Tests/test_agent_framing.cpp` covers the splitter at chunk sizes whole/7/3/1, CRLF, pings, event-without-data,
multi-line data, mid-frame EOF.

---

## 6. Open questions

**For B (server)**
1. **Frame typing.** Plan 1.3 shows `event: round` lines but says "same framing as /api/chat-stream", whose frames
   are `data: {"type":...}` with no event lines. The client accepts both; please pick one and write it in the
   contract. (If event lines: note `EJStreamFraming` would drop them, which is why `EJAgentFraming` exists.)
2. **Does the response END after `await`?** The client treats `await`/`done`/`error` as the terminal frame of a
   response and stops reading; if the server keeps the socket open after `await`, the client still works (it stops
   reading), but please confirm so the stall watchdog is not misread.
3. **`talk(ask)` shape.** I read `args.ask = {question, choices:[ "label" | {label, intent?} ]}` and answer
   `{choice, typed}`. Confirm, and whether a typed free-text answer is acceptable as the choice.
4. **No stop endpoint.** Stop is client-side only; the session TTLs out (1 h). If B wants `POST /api/agent/stop`,
   it is one call in `EJAgentClient::stop()`.
5. **"Edited" taps** (plan section 5: `edited` with the user's values). Not built: the plan card has approve/skip
   per line only. Editing an op's values needs a field editor per op shape; proposing it as a phase-2 item.
6. **Start body**: `{goal, context, agentMode:true, appVersion, chatId}`. `context` is the executor's; shape TBD
   with A. Is `chatId` wanted for the transcript store?
7. **Soft cap "keep going?"** - assumed to arrive as `talk(ask)`; nothing special built.

**For Sean**
8. The **60 s playback timeout** and the **1 s "something was heard" floor** are my numbers (plan 1.4 suggests
   60). Both are constants (`playbackTimeoutS`, the `>= 1.0` in `endPlayback`).
9. **Stack or exclude** the ask shelf and the agent card (E3). I recommend exclude.
10. **Where talk text lives**: streamed into the card while the round runs, then ONE assistant bubble per round in
    the transcript when the step goes out. The card is the live view; the transcript is the record.

**For A**
11. `EchoJayAPI::transportEndpoint`'s exact signature (private static; I read the call, not the definition).
12. Whether `appendLocalResultBubble`'s workspace append is wanted for agent lines (it persists them to the chat
    store, like the loop's bubbles do). I think yes: the record should survive a reopen.
13. The executor's `startContext()` shape — I suggest reusing `buildCurrentChainInjection`'s data, not its prose.

---

## 7. What I did NOT establish
- Anything about compilation. The JUCE calls are the ones the tree already uses (`FontOptions`, `TextLayout`,
  `WebInputStream` one-byte reads, `ListenerList`, `SafePointer`-free design), but no compiler has seen these files.
- The real server's behaviour: B's endpoints do not exist yet (saas `hold/agent-day1` has the levelling contract
  and fixes, not the loop). The recorder in the guard is the only server these files have met.
- Whether `juce::TextLayout` breaks a single over-long token (a 400-character plugin name) mid-word. Word wrap is
  asserted; a token wider than the column is the one case the measure cannot save, and plugin names are short.
