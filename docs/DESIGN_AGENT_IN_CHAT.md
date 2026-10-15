# Item 0 — the agent in the chat, and item 0b — the run survives the window

**Design, 14 Oct 2026. Nothing here is built.** Sean's ruling: *"like Claude does it."* Written while gate10
runs, so it describes the tree as it is after this round's eight fixes.

**RULED, 14 Oct:** Sean agreed every recommendation in §7 (Q1-Q8) — so they are decisions now, not
proposals: the finished message collapses to its closing sentence, per-line Apply plus Apply all, Stop on
the running message only, auto-scroll once at the start, the floating panel kept one round then deleted, one
turn at a time per chat, an interrupted run reopening as a finished row that does **not** resume, and
Undo all on the run's message greying out honestly. He also ruled **AUTO mode** (§6), and the build order:
**item 0 + 0b with Auto, then 0a.**

---

## 1. The one fact that decides the shape

The transcript **already renders structured cards inside messages, with buttons**. `ChatMsg`
(`PluginEditor.h:1883`) carries typed payloads beside its text:

| field | what it renders today |
|---|---|
| `chainData` | the proposed chain's rows, with a **Build** button that appears only when the block closes |
| `chainProvisional` | rows streamed so far, **no button yet** — "a stream that dies here leaves rows and no button, which is the honest state" |
| `editData` | a chain edit, with Apply |
| `gainData` | a gain move |
| `askData` + `askAnswered` | the model's question as **chips in the message**, disabled once tapped |

So "the agent's plan is a message with buttons on it, and its steps tick off in place" is **the idiom this
chat already has**, used four times. Item 0 is a fifth payload kind, not a new mechanism. That matters for
Sean's downgrade rule as well: a new key in the persisted row is simply never read by an old build
(`LinkShm.h:2063`, the same rule the sidecar follows — *"a new key at v:1 is simply never read by an old
reader"*), so a project saved by the new build opens in `ship_2026-10-09a` showing the agent's closing
sentence as ordinary text.

## 2. What an agent turn looks like in the transcript

```
┌────────────────────────────────────────────────────────────┐
│ you                                              09:41     │
│ make the vocal sit better against the drums                │
└────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────┐
│ EchoJay                                          09:41     │
│ The vocal's fighting the drums around 300 Hz. I'll dip     │
│ that, add a compressor and match the level back.           │
│                                                            │
│  ✓  read the vocal's rack                        0.4 s     │
│  ✓  dip 300 Hz by 2.5 dB on the EQ               1.1 s     │
│  ◐  add EchoJay Compressor after the EQ                    │
│  ·  match the level back                                   │
│                                                            │
│  ⏹ Stop                                                    │
└────────────────────────────────────────────────────────────┘
```

A turn that needs a decision puts the buttons **on the lines, in the message**:

```
┌────────────────────────────────────────────────────────────┐
│ EchoJay                                          09:44     │
│ Here's what I'd do. The second one replaces a plugin, so   │
│ I've left it for you to confirm.                           │
│                                                            │
│  ✓  dip 300 Hz by 2.5 dB                         applied   │
│  ?  replace the compressor with Pro-C 2      [Apply][Skip] │
│  ?  match the level back                     [Apply][Skip] │
│                                                            │
│  [ Apply all ]   [ Skip all ]                              │
└────────────────────────────────────────────────────────────┘
```

And when it finishes, the same message closes with its own sentence — no second bubble:

```
┌────────────────────────────────────────────────────────────┐
│ EchoJay                                          09:44     │
│ ...                                                        │
│  ✓ dip 300 Hz  ✓ Pro-C 2  ✓ level matched                  │
│                                                            │
│ Done. The vocal's 2.5 dB down at 300 Hz with Pro-C 2       │
│ after the EQ, and the level matched back to where it was.   │
│                                            [ Undo all ]    │
└────────────────────────────────────────────────────────────┘
```

**Rules of the rendering**, each one because of something that has already gone wrong:

- **one message per run.** The plan, the ticks, the approvals, the results and the closing sentence are the
  same row. Sean's item 6 was the window disagreeing with itself; two places telling the same story is how
  that happens.
- **steps tick in place.** Statuses are the glyphs the panel already draws (`EJAgentPanel::paintGlyph`:
  spinner, tick, cross, skipped, undone, stopped). No new vocabulary.
- **a line that failed keeps its own words.** The failure's user sentence sits under that line, and
  **Details discloses the `message` field** — Sean's item 3 was Details doing nothing, so in the new shape
  Details is a disclosure triangle on the line, not a button that has to find a dialog.
- **the buttons are per line, plus Apply all.** Sean's item 2 was a single Apply recording the line as
  *skipped* and the line losing its own Apply. In this shape the line's Apply is the primary control and
  "Apply all" is the convenience, which is the opposite of the arrangement that broke.
- **nothing claims a landing it has not read.** The tick appears when the read-back confirms, not when the
  write returns — this round's rule, now in nine sinks and the undo op.

## 3. How it lives beside normal chat

- **one turn at a time per chat.** An agent run and a chat reply are both "a turn"; the composer disables
  with the same notice either way. Interleaving them would make `chatId` ambiguous, which is what item 5's
  keep-rule bug grew out of.
- **a chat turn is unchanged.** Plain text in, plain text out, and the four existing card kinds still work.
  Agent mode OFF keeps Sean's standing arrangement exactly: today's chat plus Apply.
- **the working channel still governs.** A run started on a Link's chat belongs to that Link's transcript
  (`workingOnUid`), which is where item 7's `syncAgentTarget` now points the executor.

## 4. What happens to the panel

`EJAgentPanel` (~1,100 lines of painting and nine buttons) becomes **the renderer for one message** rather
than a floating card: its `paintContent`, `paintGlyph` and button styling move into a transcript row
component, and `onLayoutNeeded` becomes the row's height request. The client's listener interface
(`addListener`/`removeListener`) is already the right shape for a row to subscribe to.

**I recommend keeping the floating panel behind Agent mode for one round**, deleted once Sean signs off —
his standing instruction is that the switch stays until then, and a renderer swap is exactly the change
that wants a fallback.

---

## 5. Item 0b — the run survives the window

### What is already true

Item 5's fix built this pattern and the rig proves it: the **processor** keeps a turn that is in flight, and
the next editor **adopts** it. `chatTurnOwner_`, `parkedChatIds_` and `drainOrphanedChatTurns` are that
machinery, and the keep-rule now asks *who sent the turn* rather than *is an editor attached*. 0b applies
the same pattern to an agent run.

### What has to move, exactly

| thing | owner today | owner after |
|---|---|---|
| `EJAgentClient` (state, steps, session, rounds, SSE) | editor (`agentClient_`) | **processor** |
| `ExecutorDo` / `ExecutorRead` | editor (`agentExecutor_`) | **processor** |
| `Sources` (rack reads, tracks, inventory) | editor lambdas | processor — they already read `processorRef` and `api` |
| the eight `DoSinks` | editor lambdas | processor, **except one** (below) |
| `EJAgentPanel` | editor | editor (it is the view, and should stay there) |

**The whole difficulty is one sink.** Of the eight, seven already reach only `processorRef` or `api`:
`applyChainOps` (via `writeChainEditCommand` / `awaitChainAck` / `getChainHost().applyChainEdits`),
`setRackOutDb` (`getBusGainDb`/`setBusGainDb`), `undoOne` and `undoHistoryStep` (`undoHistory()`),
`agentModeOn` / `echoJayOnly` (`api.getEchoJayOnly`), `afterMs` (a timer).

`buildChain` is the exception: it calls **`loadChainFromJson`** and **`sendChainToLink`**, both large editor
methods. Two honest ways:

- **(recommended) move the two build roads to the processor.** They are the same choke point this round's
  item 1 created, so there is one place to move, and a build is a processor-level act anyway — it writes the
  chain command, instantiates plugins and arms the loudness loop (`armLoudnessAfterBuild`), none of which is
  about a window. Cost: a real refactor of two long methods, and they touch editor UI state (selection, the
  strip) which has to become a notification the editor reacts to.
- **(cheaper, worse) keep `buildChain` editor-bound and have the run pause when no editor is attached.** A
  run would survive the close but **stall at a build** until the window reopens. I do not recommend it: "the
  run survives the window" that silently excludes builds is the kind of half-truth this round has been
  removing from the sinks.

### FIRST, A CORRECTION TO EVERY LEG BELOW (15 Oct 2026)

**This host does not destroy the plugin's editor when the rig closes a window, so none of the legs below
can be written the way they are written here.** Found while chasing the C11 row: the rig releases the AU's
wrapper view, its own `unique_ptr<AudioProcessorEditor>` goes null, `v2.editor == nullptr` passes - and the
plugin reports the **same `EchoJayEditor` alive for the whole run**. One editor is built; one destructor
runs, at the end of the process. The proof is in the plugin's own log: `EJEditor: window #1 built`, and a
single `EJCmp: streams stopped on the editor closing` at the last second of the run.

Three consequences, all of which this round has acted on:

1. **Two rows were passing vacuously.** R4 ("closing mid-reply keeps the user's message") and the first cut
   of C11 asserted on a window that had never gone away. The user's message survived because nothing had
   died. Both rows now say what they actually prove, and the destroyed-window half is a **named skip with
   its reason** rather than a silent pass - the same rule as the vacuous-pass gate.
2. **The rig gained a second channel it can read with the window shut:** `rig/rig-processor.json`, written
   by the **processor** on its 1 Hz timer (`editorAlive`, `editorId`, `editorsBuilt`, `chatOwnerId`,
   `parkedChats`, `orphanedTurns`, `historyRows`, `lastAssistant`). `rig-state.json` is published by the
   *editor* when it is given a command, so with no window there is nothing to ask and nothing to read -
   which is precisely the state every one of the legs below is about. `Track::closeEditor()` now waits,
   bounded, for the plugin to report `editorAlive:false` and records whether it ever did.
3. **Before item 0b is built, the rig needs a host that can really close a window.** Two candidates, and
   the first is cheap: load the **VST3** for these rows (JUCE's VST3 host releases `IPlugView` by COM
   refcount, which deletes the plugin-side editor deterministically, where the AU hands back an autoreleased
   NSView whose lifetime the host does not own); or drive the AU's view factory directly in the rig rather
   than through `createEditorIfNeeded`. **Until one of them is in place, an 0b leg can only be written as a
   skip, and writing it as a pass would be worse than not writing it.**

### What the rig will assert

New legs, each on real state, in `tools/host_rig/`:

1. **close mid-run, reopen, the run finished meanwhile.** Start a multi-step goal; close the editor while
   `state == Executing`; wait for the server to finish; reopen. Assert: the rack holds every change the run
   made (read back), the transcript holds **one** agent message for that run with every step at its final
   status, and the closing sentence is present.
2. **close mid-run, reopen while it is still going.** Same, reopening while `isActive()`. Assert: the
   reopened window shows the **same run** still ticking (same `sessionId`), no duplicate message, and the
   steps continue to final statuses afterwards.
3. **approvals survive the close.** Run to `AwaitingApproval`, close, reopen, press the line's Apply.
   Assert: the step runs and the rack reads back the change — the plan was not lost and was not
   auto-applied either.
4. **an ask survives the close.** Same at `AwaitingAsk`, answering after the reopen.
5. **Stop after a reopen** halts the run and leaves Undo all reachable (`Stopped`, not `Failed`).
6. **no editor at all for the whole run.** Start, close, let it finish with nothing attached, reopen.
   Assert the same as leg 1 — this is the leg that fails today, because the client dies with the editor.
7. **two windows.** Open a second editor mid-run; assert both show the same single run and the run is not
   restarted (the keep-rule bug in a new costume).
8. **project close mid-run** → see question 7.

---

## 6. AUTO MODE (Sean's ruling, 14 Oct 2026) - **BUILT, 15 Oct, and gated**

**What landed, and where it differs from this section.**

- the chip cycles **Off -> Ask -> Auto -> Off** with the mockup's words, colours (grey / amber / accent)
  and tooltip, and the plugin publishes `agentApprovalMode` so a rig row can assert which state it is in.
- the mode is **persisted in the existing flag file's TEXT** ("ask" / "auto"), with the file's EXISTENCE
  still the on/off truth. So `agentModeOn()`, every guard and `ship_2026-10-09a` are untouched, and an old
  build reads "agent on" for both - which is true. Anything else in the file (today's "on") reads as Ask.
- `ExecutorDo::undoabilityOf(call, goal)` is on the **ToolExecutor interface**, with the safe default (not
  undoable, so Auto asks) for any executor that has not been taught the question. The real one answers op
  by op, and `agent_round15_guard` holds it against the tokens the ops actually return.
- `approvalMode` rides the start body, sent only when set.
- **ONE DEVIATION FROM THE MOCKUP, AND IT IS SEAN'S TO RULE.** The mockup puts the mode line *under* the
  composer. There is no vertical space under the composer box in the real window, so the line floats
  **immediately above** it, anchored to `chatBoxRect_` exactly as the Reaper key hint does - the idiom this
  file already uses for "a strip by the composer" - and it yields to that hint. The words are the mockup's,
  unchanged. If you want it under the box instead, that is a layout change to the chat surface's height
  math rather than a floating strip, and it should be its own commit.

The rows: **A9** (an Auto run of two ask_first steps, zero approval presses, the change read back), **A7**
("undo that" in Auto with no wait), **A11** (a step that cannot be undone still waits, with its reason),
**A8** (the same request in Ask still waits), plus the chip's three states, the line's words, Off changing
nothing after Auto has been used, and the mode persisting across a window rebuild.

### The ruling, as it was written

> *"The Agent chip cycles Off -> Ask -> Auto. Ask = today's approvals. Auto = every step applies without
> waiting; the run stops only for a question (talk/ask), or for any step that cannot be undone (still
> ask-first, with the reason on the line). Server limits (the -0.1 ceiling etc.) still apply. Steps still
> tick in the message and Undo all stays on it. Default for a new user is Ask; the choice persists per user."*

### The chip

Three states, cycled by the one control, with the mode written where the agent switch already persists:

```
   [ Agent: Off ]  ->  [ Agent: Ask ]  ->  [ Agent: Auto ]  ->  Off
```

- **Off** is unchanged and stays Sean's standing arrangement: today's chat plus Apply, nothing changed
  without a press.
- **Ask** is today's agent behaviour.
- **Auto** applies each step as it comes.

`Off` must remain the state that changes nothing, so the *executor's* own gate (`sinks_.agentModeOn`, checked
live on every call, not cached) keeps refusing in Off exactly as it does now. Auto changes **when the client
waits**, never whether the executor is allowed to act.

### What Auto does and does not skip

The approval class is **the server's decision**, already on every tool call
(`EJAgentProtocol.h:15`: `"approval":"free"|"ask_first"`), and the plugin defaults a `do` to `ask_first` when
the field is absent. Auto does not discard that: it **honours `ask_first` in two cases and proceeds in the
rest**.

| situation | Ask | Auto |
|---|---|---|
| `approval: "free"` | runs | runs |
| `approval: "ask_first"`, step is undoable | waits for Apply | **runs**, line shows "applied automatically" |
| `approval: "ask_first"`, step is **not undoable** | waits | **waits**, with the reason on the line |
| a question (`talk` / ask) | waits | **waits** |
| the server declines on a limit (the −0.1 ceiling etc.) | refused | **refused, identically** |

**Undo all stays on the message** in both modes, and the steps tick in the message in both modes. Auto
removes the waiting, not the record.

### "Cannot be undone" has to be known BEFORE the step runs

Today undoability is only known *afterwards*: `Step::canUndo()` is `status == Done && undoToken.isNotEmpty()`,
and the token is whatever the executor chose to hand back. A pre-flight rule read from a second,
hand-written list would drift from it within a round — so:

**one function, two readers.** `ExecutorDo` gains `undoabilityOf(op, args) -> {undoable, whyNot}`, and it is
the **only** thing that decides both (a) whether Auto may proceed without asking and (b) whether the
outcome carries an undo token. The same discipline SHORT90's codec states for itself: *"one writer, one
reader, both here — a quantised field decoded in two places is a field that disagrees with itself."*

A guard leg holds the two together: for every op the executor knows, `undoabilityOf` and the token the op
actually returns must agree. That leg is what stops Auto quietly applying something irreversible after a
later change to one of the sinks.

**`undo` is itself the awkward case**, and I would rather name it than let it surprise us: the undo op
returns **no** token (undoing an undo is a redo, which the agent does not have). Taken literally, Auto would
stop and ask before an undo the user has just asked for out loud.

**RULED, 14 Oct:** it does not. *"'undo that' typed by the user runs without a second approval."* The user's
own words are the approval, so an `undo` on an undo goal is **pre-approved in Auto** - one stated exception
in `undoabilityOf`, and no redo op before beta. It still asks in Ask mode. Rows A7 and A8.

### The rig legs Sean named, plus the two that make them trustworthy

1. **an Auto run that completes with no approvals** — multi-step goal, mode Auto. Assert: zero approval
   waits, every change read back from the rack, and every step's final status in the message.
2. **an Auto run that stops for a question and continues after the answer** — assert it reached
   `AwaitingAsk`, that nothing ran past it while it waited, and that the remaining steps completed after the
   rig answered.
3. **a non-undoable step still asks in Auto** — assert the wait happened, the line carries the reason, and
   the step runs only after the press.
4. *(mine)* **Off still changes nothing in Auto's presence** — cycle Off→Ask→Auto→Off and assert the rack is
   untouched by a run started in Off, because a three-way control is a new way to get that wrong.
5. *(mine)* **the mode persists** — set Auto, destroy and recreate the editor, assert the chip reads Auto and
   an Auto run needs no press. Sean's ruling says it persists per user, and a setting that silently resets to
   Ask would look like Auto failing.

### What B must agree

The server owns the approval policy, so the flag has to be in the request and honoured there:

- the plugin sends the mode with each run (`"approvalMode": "ask" | "auto"`).
- in `auto`, B may mark steps `free` that it would otherwise mark `ask_first` — **except** anything
  irreversible or outside the rack, which must stay `ask_first` whatever the mode says. The plugin enforces
  its own undoability rule on top, so a disagreement is safe in the direction that asks.
- the limits do not move: the −0.1 ceiling and every other server-side refusal apply in Auto exactly as in
  Ask, and Auto must never be read as permission to exceed one.
- B should say in the frame **why** a step stayed `ask_first` in Auto, so the line can show the reason
  rather than the plugin inventing one.

---

## 7. Questions that are Sean's, with my recommendation

1. **Does a finished agent message stay expanded in the transcript?**
   *Recommendation: collapse to the closing sentence plus "3 changes ▸", expandable.* A long run otherwise
   buries the conversation above it.
2. **One Apply per line plus Apply all — agreed?**
   *Recommendation: yes, and a one-line plan's Apply means that line.* This is the direct fix for item 2.
3. **Where does Stop live?**
   *Recommendation: on the running message, and nowhere else.* A fixed bar is a second place for the window
   to disagree with itself.
4. **Does the transcript auto-scroll to a running agent message?**
   *Recommendation: once, when the run starts, and never again while it runs* — so reading back through the
   history is not yanked away each time a step ticks.
5. **Does the floating panel go in the same build, or stay behind Agent mode for a round?**
   *Recommendation: stay for one round, then delete.*
6. **May a run start while a chat reply is streaming?**
   *Recommendation: no — queue it, one turn at a time per chat.*
7. **A run still going when the project is closed: what does a reopened project show?**
   The session cannot survive the host quitting (it is a server session, and the processor is destroyed).
   *Recommendation: the message reopens as a finished row saying the run was interrupted and which steps had
   landed, and does NOT resume.* Resuming something the user has not watched is the opposite of this
   round's read-back rule.
8. **Where does the undo-all button live once the panel is a row?**
   *Recommendation: on the run's message, for as long as the history still holds its steps* — and it should
   grey out honestly when the plugin-wide history has moved past them (`undoAllowedNow`).
9. **(new, from Auto) Does an `undo` need its own approval in Auto?** — **RULED, 14 Oct: no.**
   > *"'undo that' typed by the user runs without a second approval. Redo can wait until after beta."*

   So **the user's own request is the approval**, and there is **no redo op before beta** - my
   recommendation below is superseded, and the better answer was the one that added nothing. `undoabilityOf`
   grows a single stated exception: an `undo` on an undo goal (which B already detects, and on which it
   refuses every other op) is pre-approved in Auto. It still asks in Ask mode, like everything else.
   Rows A7 and A8. The original question and my recommendation are kept below, because the reasoning for
   *why* an irreversible step asks is what makes the exception safe to carve.

   ORIGINAL QUESTION:
   Taken literally the rule says yes, because the op returns no undo token — so Auto would stop and ask
   before an undo the user just asked for out loud.
   *Recommendation: add a `redo` op in the same push and treat `undo` as undoable.* The history already
   keeps the entry on its redo stack (`UndoHistory::redo`), so the claim would be true rather than
   convenient — and "undo" being the one phrase Auto stumbles on would be a poor joke to ship.
