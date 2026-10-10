# Agent mode, plugin side — Session A2 handoff (9 October 2026, branch `feat/agent-ui`, worktree `~/echojay-vst-a2`)

**Status: WRITTEN, NOT COMPILED. Matched to B's `docs/CONTRACT_AGENT_TOOLS.md` (saas branch `hold/agent-contract`) in the second commit.** A2 never compiles, gates, installs or runs Logic (EJ Map was scanning; A owns
every build on this Mac). Every file below was written against the headers it includes, read today, but the first
compiler to see them is A's. Expect the usual first-compile fixes; the design will not change under them.

Read alongside: `docs/LINK_REMOTE_CONTROL_PLAN.md`, `HANDOFF_COMP_PROFILES_PLUGIN.md` (08c),
`~/echojay-saas/docs/AGENT_MODE_PLAN.md` and **`~/echojay-saas/docs/CONTRACT_AGENT_TOOLS.md`** (B, branch
`hold/agent-contract`), which is the source of truth for every wire shape in `EJAgentProtocol.h`.

---

## 1. Files (all NEW; nothing existing was edited)

| file | what | depends on |
|---|---|---|
| `Source/EJAgentFraming.h` | SSE splitter that KEEPS `event:` names. `EJStreamFraming` drops them by contract (chat-stream never sends named events); the agent frames are typed by them and by nothing else (contract 4). JUCE-free, standalone-testable. | nothing |
| `Source/EJAgentProtocol.h` | the wire vocabulary as pure functions: `parseFrame(eventName, json)` (type from the event line ONLY), the `plan` frame, `ToolCall`, `ToolResult` (Skip = `approval_declined`), `buildStepBody`, `buildStartBody`, `buildStopBody`, `describeCall` (incl. `open_editor`), `isPlaybackWait`, `playbackTimeoutResult` (the contract's sentence, verbatim), `talkAskOf` (choices `{label, detail?, intent?}`, `allowFreeText`), `askAnswerVar` (`{tapped}` / `{typed}`). Header-only, `juce_core` only. Default approval when a frame carries none: `do` asks, everything else free (the safe direction). | juce_core |
| `Source/EJAgentTools.h` | **the executor interface A implements** (`echojay::agent::ToolExecutor`) + `StubExecutor` (answers `not_in_phase` to everything, counts no playback, has no checkpoint: honest, so the loop and cards are exercisable before A's lands). | EJAgentProtocol.h |
| `Source/EJAgentClient.h/.cpp` | the loop driver: `/api/agent/start` + `/step` as SSE POSTs (one-byte reads, cancel handle, `net::Worker` census entry, callAsync behind alive + generation), rounds, free calls first then the plan card then the approved lines in wire order, `talk(ask)`, `wait_for_playback` (client-run, 60 s, never hangs), Stop (`POST /api/agent/stop`, in-flight calls marked stopped, nothing else posted), Undo per step / Undo all, Retry, a 90 s stall watchdog. Renders nothing. | EJAgentFraming, EJAgentProtocol, EJAgentTools, `EJNetCensus.h` |
| `Source/EJAgentPanelLayout.h` | the panel's geometry as pure functions (`wrappedTextHeight`, `layoutRows`, `layoutChips`, `checkRows`). One author for paint and buttons; what the guard asserts. | JuceHeader |
| `Source/EJAgentPanel.h/.cpp` | the card: header (status dot, status line, **Stop**), scrolling body (goal, streamed talk, notice, the plan frame's **heading**, **the checklist** with glyphs + the server's summary + landed line/error + per-row **Apply/Skip** toggles while the plan waits and **Undo** once allowed, the **ask** question + choice pills, the **playback** ring + sentence), footer (**Apply all / Apply / Skip all**, **Got it, you can stop**, **Undo all / Retry / Close**). EchoJay palette (`EchoJayLookAndFeel::Colours`), the chat's 26 px pills, `kFieldCorner`. | EJAgentClient, EJAgentPanelLayout, `EchoJayLookAndFeel.h` |
| `Source/EJAgentExecutorRead.h/.cpp` | **the READ-ONLY half of the executor** (`echojay::agent::ExecutorRead`): `look` (rack / get_rack, channel / list_tracks, analysis / analyse, levels, inventory; maps and saved_chains answer `server_tool`), `check` (level / measure, gr, true_peak, spectrum, balance, compare_to_checkpoint), `captureCheckpoint`, the playback window (`beginPlaybackWindow` resets ONLY the loop-owned tallies, exactly `LoudnessLoop::startWindow`'s list; the song's integrated reading is never touched), `readPlayback`. `doOp` / undo answer `not_in_phase` for A. Reads through `Sources` (functions): `bindToProcessor(proc)` binds them to ChainHost + its tallies + MeterEngine for the own rack and to the registry row + `LinkShm::readRackSidecar` + `readLinkMeterFrame` / `linkLastGoodFrame` for a Link (LINK_REMOTE_CONTROL_PLAN.md section 5: files plus shared memory, read-only). Every result passes `fitUnder8K`. | EJAgentTools, PluginProcessor, ChainHost, LinkShm, MeterEngine, LoudnessLoop |
| `tools/agent_executor_read_guard/harness.cpp` | G1-G16 against a fake host (section 5). | the V2 archive |
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
    Source/EJAgentExecutorRead.cpp
    Source/EJAgentExecutorRead.h
```
and in `tools/tests/CMakeLists.txt`, with the fast guards:
```
ej_add_guard(agent_client_guard        SOURCE tools/agent_client_guard/harness.cpp              LABEL fast)
ej_add_guard(agent_executor_read_guard SOURCE tools/agent_executor_read_guard/harness.cpp       LABEL fast)
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
    // the read-only half (A2); A's mutating half replaces doOp / undo on the same object or wraps it
    auto sources = echojay::agent::bindToProcessor (processorRef);
    sources.inventory = [this] { return /* E2b: the installed names the scanner / recommendable feed holds */ juce::StringArray(); };
    auto readExec = std::make_unique<echojay::agent::ExecutorRead> (std::move (sources));
    agentReadExecutor_ = readExec.get();                                  // keep a typed pointer for setTarget
    agentExecutor_ = std::move (readExec);
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
        if (agentReadExecutor_) agentReadExecutor_->setTarget (activeChatLinkUid());   // "" = this rack, else the chat's Link
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

## 2b. Levelling is a RACK-LEVEL record (Sean, 10 Oct 2026)

The EchoJay Level slot is dropped. Levelling drives the rack OUT gain (`match`) or the final limiter's IN gain (a
target), and its record is stored at rack level. The read-only executor REPORTS that record and never looks for a
slot: `look(rack)` carries a top-level `levelling {option, targetLufs?, drives: "rack_out"|"limiter_in",
landedGainDb, inLufs, outLufs, deltaDb, converged, state?}`, `check(level)` carries `option / targetLufs / drives /
landedGainDb / converged` beside the window's figures, `startContext` carries `levelling {option, targetLufs}`, and
`compare_to_checkpoint` reports `landedGainThenDb / landedGainNowDb / landedGainDeltaDb` and `optionChanged`.

**The seam for A** is two lambdas in `bindToProcessor`: `ownLevelling` (today: the loop's `loudnessOption()` mapped
to the contract's option, `target()`, `currentGainDb()`, `everArmed / isArmed / hasProposal` for converged, and the
stored `LevelRecord`'s INT for the chain input) and `linkLevelling` (today: the sidecar's `levels` record for the
Link's INT, and the rack-level keys `option / target / drives / landedDb / inLufs / converged` read from that var when
A publishes them). When the record lands, those two lambdas change and nothing the executor sends changes shape.

**B's contracts had NOT changed as of 10 Oct 10:00** (`CONTRACT_LEVEL_PARAMS.md` and `CONTRACT_AGENT_TOOLS.md` on
`hold/agent-contract` still describe the Level slot, `set_level`, the `level_contract` validator and `check(level)` as
"the Level slot's own reading"). The executor's shape above is what the plugin can state today; B's rewrite should
name `levelling` at rack level and retire `set_level`'s "added if absent" clause.

## 3. The executor contract (what A's class implements; `Source/EJAgentTools.h`)

**Built by A2 (read-only, `Source/EJAgentExecutorRead.h/.cpp`):** `startContext`, `look`, `check`,
`captureCheckpoint`, `beginPlaybackWindow`, `readPlayback`. **Left to A:** `doOp` (every op in contract 2.2 plus
`open_editor`, over the chain / control / ring channels of `docs/CONTRACT_LINK_COMMANDS.md`), `undoStep`,
`undoToCheckpoint` (the plugin-wide history; the read half's checkpoint token can be the undo checkpoint too, or A
keys its own). The member names the binding reads were checked against the 9 Oct headers (`getChannelType`,
`getCustomChannelName`, `getProjectName`, `getLinkSlotInfos`, `readLinkMeterFrame`, `linkLastGoodFrame`,
`resolveLinkDisplayName`, `getChainHost`, `getMeterEngine().getMeterData()` / `reduceMacroWindow`,
`ChainHost::getSlotInfo / slotPicture / dialSummaryRow / isBuiltinSlot / getChainInLoopLevels / getChainOutLevels`,
`LoudnessLoop::kCountFloorLufs`, `LinkShm::resolveDir / readRackSidecar`); the first compile will say if any moved.

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
- **A round**: `round` (with `ofSoftCap`) resets the round's talk; `delta`s stream into the card's text; each
  `tool_call` is a row at once; the `plan` frame marks its items (the card's lines, in its order) and carries the
  heading; `await` (terminal: the server closes the response) starts execution. EVERY FREE CALL RUNS FIRST, in wire
  order (contract 3: "free calls in the same round run before the card is shown"); then the plan card; then the
  applied lines in wire order. A failed `do` marks the round's later `do`s `not_run` (looks still run). When every
  awaited id is answered, ONE `/step` is posted and the round's talk goes to the transcript once. A result over
  8 KB is logged as a warning (the server rejects it with `result_too_large`).
- **Plan card** = the plan heading + the checklist's plan rows with Apply/Skip toggles + footer Apply all / Apply /
  Skip all. Skipped lines return `{ok:false, error:{code:"approval_declined"}}`. No Edit (per Sean's ruling).
- **talk(ask)**: the question + choice pills (`{label, detail?, intent?}`); a tap answers `{tapped:"<label>"}`,
  a typed message `{typed:"<text>"}` when `allowFreeText`, else the typed message is refused with "Tap one of the
  choices to answer." The soft cap is an ordinary ask with id `tc_keep_going`.
- **wait_for_playback**: "Play the chorus or the loudest section." + a ring that fills toward `min_seconds`;
  "Listening... n of m s"; at `min_seconds`: "Got it, you can stop." and `{played:true, seconds, integratedLufs,
  loudestLufs, peakDbtp}` goes back; the user's **Got it** returns what was heard (`short:true` when under); a
  known-stopped transport after >= 1 s heard ends it early; the **60 s timeout** returns the contract's answer
  verbatim: `{played:false, waited:60, sentence:"Nothing played in 60 seconds - play the loudest section and ask me
  again."}`, and the row says the same sentence. It cannot hang: every exit is on the client's own timer.
- **Stop**: generation bump + socket cancel; every in-flight row reads stopped; `POST /api/agent/stop {sessionId}`
  goes out (fire and forget, logged); NO step is posted; a late executor completion or frame is ignored; "Stopped."
  in the transcript; the card stays so Undo is reachable. `done.reason` ("complete" | "stopped" | "hard_cap" |
  "error") is kept on the client.
- **open_editor** `{slot}` is a free `do` the executor handles (the Link opens its floating window); its row reads
  "Open the editor for slot n".
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
| P1-P15 | typed by the event line ONLY (a JSON `type` is not a type); terminal frames; a bare `do` asks; step/start/stop bodies; `approval_declined`; the plan frame; round/await/done extras; talk(ask) choices + allowFreeText; `{tapped}`/`{typed}`; the timeout answer verbatim; `open_editor`'s line |
| N1-N6 | `nextRunnable`: every free call first (even behind a plan line), then -2 on an undecided plan, applied lines after submit, nothing while a step runs |
| R1a-n | start body; checkpoint; goal as the user's turn; deltas; rows; the plan frame's heading; the look AND the check run before the card; ONE step, every id, await order; talk once; undo token on the landed do; log lines |
| R2 | Skip all -> `approval_declined`, nothing ran |
| R3 | per-line Skip -> `approval_declined`; applied line runs; order kept |
| R4 | failed do -> later do `not_run`, look still runs; rows carry the reasons |
| A1-A7 | ask card with details; typed answer `{typed}`; `allowFreeText:false` refuses typing with a line and ignores a tap that is not a label; `{tapped}`; the soft cap `tc_keep_going` |
| W1-W4 | timeout returns `{played:false, waited, sentence}` verbatim; min reached returns the reading + "Got it, you can stop."; early stop `short`; stopped transport ends it |
| S1-S7 | Stop at once; `POST /api/agent/stop {sessionId}`; rows Stopped; no step; late completion + stale frame ignored; "Stopped."; Undo reachable |
| U1-U3 | undo per step sends its token; undo all sends the checkpoint; notice |
| E1-E8 | error frame; Retry byte-identical; done + reason + summary; dismiss; EOF -> Failed; no token -> Sign in |
| T1-T2 | typed under a running agent refused with a line; passes through otherwise |
| L0-L8 | the panel at 6 widths x 4 height caps in 4 scenes (plan card with heading, ask with six chips, listening, done) passes `checkLayout`; chips wrap at 380; a short cap scrolls, never cuts; re-dock asked for; hidden after dismiss |

`tools/agent_executor_read_guard` (fast), the read-only executor against a FAKE host (every `Sources` function is the
guard's):

| leg | what is RED without the behaviour |
|---|---|
| G1 | startContext: `channel {uid:self, name, kind, links[]}`, capabilities, agentMode, `levelling {option, targetLufs}`; a Link target names the Link |
| G2 | look(rack) / get_rack: 1-based n; name / bypassed / wet % / keepLevel / builtin / settings / inDb / outDb / grDb; built-in role; settings capped at 160; the RACK-LEVEL `levelling` record and NO Level slot |
| G3 | look(rack, channel:uid) reads the Link's sidecar, remote:true; unknown uid -> `unknown_channel`; a Link target makes self the Link |
| G4 | look(channel) / list_tracks: the registry rows with audio / placement / gone / gainDb |
| G5 | look(analysis) / analyse: integrated, loudest 3 s, peak, PSR, overs, heard, playing, six `{band, vsAverageDb}`; no audio -> `not_playing` + hint; a Link's from its frame |
| G6 | look(levels): per slot or "none"; chain in / out / delta from the loop's tallies |
| G7 | 400 inventory names -> under 8 KB, `truncated:true`, count 400; unbound -> `inventory_unavailable` |
| G8 | maps -> `server_tool`; unknown what -> `unknown_what` |
| G9 | check(level) / measure: in / out / delta / loudest 3 s / peak, `source`, plus `option / targetLufs / drives / landedGainDb / converged` from the rack-level record; nothing heard -> `not_playing`; a Link target reads the frame and its record |
| G10 | check(gr): the slot's GR; out of range -> `unknown_slot` with the range; no reading -> `not_playing` |
| G11 | check(true_peak): the last slot's peak + overs, or a named slot |
| G12 | check(spectrum): six bands; check(balance): Link vs this mix bus, else `no_mix_reading` |
| G13 | checkpoint + compare: unchanged -> false / 0; a bypass + 1.5 dB -> true / 1.5; the landed gain then / now / delta; newest when unnamed; unknown token; a Link compares frames |
| G14 | the window opens ONCE and never resets the song's reading; readPlayback from the out tally; a Link counts beyond its base |
| G15 | doOp / undo -> `not_in_phase` |
| G16 | a 16-slot rack with 300-char settings and every result in the guard under 8 KB |

`Tests/test_agent_framing.cpp` covers the splitter at chunk sizes whole/7/3/1, CRLF, pings, event-without-data,
multi-line data, mid-frame EOF.

---

## 6. Open questions (after the contract)

**Answered by `CONTRACT_AGENT_TOOLS.md` and Sean's 9 Oct message, and built:** frame typing (event line only),
terminal frames, `talk(ask)` shape and answers, Stop endpoint, start body, the soft cap, no Edit on the plan card,
the playback timeout answer, `open_editor`.

**Still open**
1. **Edit on the plan card.** The contract's section 3 names Apply / Edit / Skip and an `edited:true` result; Sean's
   message says Apply / Skip and no "edited". Built as Sean said. If Edit comes back it is a field editor per op
   shape plus one result flag - phase 2.
2. **Resume** (contract 1: a reconnecting client POSTs `/step` with the last round and no results). Not built; the
   client holds `sessionId` and `round`, so it is one call when wanted.
3. **`context` on start.** The contract puts `agentMode: true` INSIDE `context`; Sean's message puts it at the top
   level. Built at the top level; the executor's `startContext()` can carry it inside too at no cost (A's call).
4. **Stop's step result.** The contract also allows answering pending calls with `{code:"stopped"}` in a step. Built
   as Sean said: `/stop` is posted and nothing else goes out after it.
5. **For Sean:** the 1 s "something was heard" floor on an early stop or a stopped transport is my number; the
   60 s timeout is the contract's.
6. **Stack or exclude** the ask shelf and the agent card (hook E3). I recommend exclude.
7. **For A:** `EchoJayAPI::transportEndpoint`'s exact signature; whether agent lines persist to the chat store
   (I think yes); the inventory binding (E2b: the installed names live with the scanner, not the processor).
8. **Names (for B).** Sean's brief names `list_tracks`, `get_rack`, `analyse`, `measure` and
   `compare_to_checkpoint`; B's contract names `look(what: rack | channel | analysis | levels | inventory)` and
   `check(what: level | gr | true_peak | spectrum | balance)`. The executor answers BOTH spellings (the aliases map
   onto the contract's shapes; `list_tracks` answers `{count, tracks[]}` and `compare_to_checkpoint` is new).
   One of the two lists should go into the contract so the model is told one vocabulary.
9. **`look(levels)` fields.** The contract says `inDbfs / outDbfs`; the plugin's per-slot picture holds LUFS and
   dBTP (`slotPicture`), so the executor sends `inLufs / outLufs / inDbtp / outDbtp / grDb`. B to confirm or rename.
10. **`check(balance)`** needs the session's target to be a Link and THIS instance to sit on the mix bus (the two
    readings come from two hosts, no sample alignment assumed - plan section 5); any other arrangement answers
    `no_mix_reading`.

## 7. What I did NOT establish
- Anything about compilation. The JUCE calls are the ones the tree already uses (`FontOptions`, `TextLayout`,
  `WebInputStream` one-byte reads, `ListenerList`, `SafePointer`-free design), but no compiler has seen these files.
- The real server's behaviour: B's endpoints do not exist yet (saas `hold/agent-day1` has the levelling contract
  and fixes, not the loop). The recorder in the guard is the only server these files have met.
- Whether `juce::TextLayout` breaks a single over-long token (a 400-character plugin name) mid-word. Word wrap is
  asserted; a token wider than the column is the one case the measure cannot save, and plugin names are short.
