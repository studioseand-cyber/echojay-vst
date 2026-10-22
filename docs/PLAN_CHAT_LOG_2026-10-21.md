# Plan: one stored log per chat (Sean's ruling, 21 Oct 2026, 14:2x)

> *"The chat keeps breaking (order after reopen, cards in the wrong chat, Stopped lines in the wrong
> chat, chips lost) because it's rebuilt by merging several copies ... and matching rows by role and
> content, which an agent row can't match. Same fault class as the rack mirror."*

He is right, and it is the same class: **several copies of one fact, reconciled by guessing.** The
rack had the mirror vs the sidecar; the chat has four copies and a cursor.

## 1. What is there now, and today's four failures in those terms

Four containers hold the same conversation:

| copy | lives on | written by |
|---|---|---|
| `chatMessages` | the editor (window) | every append, the replay, the merge |
| `chatHistory` / `chatRoles` / `chatContents` | the processor | the same appends, rebuilt by the merge |
| the workspace store (disk) | `EchoJayWorkspace` | `appendMessageToChat`, the mutation sync |
| the run's snapshot (`agentData`) | on a row, and again on the processor's entry | `syncAgentRunMessage`, `EJAgentHome::writeSnapshot` |

On reopen they are reconciled by `openChatById`'s merge (PluginEditor.cpp ~20200-20330): it walks the
processor's rows against the store's with a moving cursor `h`, matching **plain rows by `role` +
`content`** and agent rows by `runKey`, inserting at the cursor whatever the store has not caught up
with. Its own log line admits the shape of the problem:

```
14:15:19  EJStores: merged 2 row(s) the workspace store has not caught up with back into their
          own place in the transcript
```

- **Order (his 14:15 item 1).** His chat held two identical `add a compressor` rows. An agent card
  has no `content` to match on, so its place comes from wherever the cursor stood when its key was
  recognised - and with a repeated user message the cursor can be one row early. That is precisely
  what he read: *"add a compressor", "add a delay", [card], "add a compressor"*. **No row knows its
  own position**, so the position is recomputed, and a recomputed position can be wrong.
- **Chips (his 14:15 item 2).** Proven from the log's own ORDER, and it is not where I looked
  yesterday:
  ```
  14:15:18  EJAgentRow: this window ADOPTED the run's row ... chips=3 in the snapshot,
            clientPendingAsk=3 label(s)        <- both copies HAD the chips
  14:15:19  EJStores: merged 2 row(s) ...      <- the replay then rebuilt chatMessages
  ```
  The adopt sets `agentRunMsgId_` to the matched row's `agentRunId`. The replay that runs *after* it
  reissues every restored row's id (`cm.agentRunId = ++agentRunIdSeq_`), so the id the editor is
  holding no longer names any row. `agentRunViewFor` then reads
  `live = hasSession() && msg.agentRunId == agentRunMsgId_` as FALSE, calls `makeRecord()`, and a
  record lays out **no chips at all** (`if (v.fromRecord) continue;`). The data was never lost: the
  row was declared history. A per-window counter standing in for a run's identity - the same proxy
  fault as slot index for slot identity.
- **Cards in the wrong chat / "Stopped." in the wrong chat.** Both were a row's chat being read from
  an editor member rather than from the row. Patched twice this week (`runChatId()`, `target`), each
  time in one more place.

## 2. The design

**One log per chat. Every entry is created once, with its own identity, and never placed again.**

### The entry

```
ChatEntry {
  juce::String chatId;      // the chat it belongs to. Stamped at creation, never derived.
  juce::int64  seq;         // monotonic within the chat, assigned at creation, never renumbered.
  juce::String id;          // stable, unique, created with the entry (the run's key for a card).
  juce::String kind;        // "user" | "assistant" | "agent_run" | "notice" | "system_line"
  juce::String content;     // the prose, for every kind that has any
  juce::var    payload;     // kind-specific and self-contained: the run snapshot (steps, chips,
                            // closing, answers), the notice's level, the capture's waveform ref
  juce::int64  createdMs, updatedMs;
}
```

Three rules, and every one of today's failures is one of them:

1. **`seq` is the order.** The view sorts by `seq` and renders. Nothing is "inserted into its own
   place", because the place is a field.
2. **`id` is the identity.** A live run owns the entry whose `id` is its run key, and updates it in
   place - steps, chips, closing, answers. No positional insert, no role/content matching, no
   per-window integers anywhere near it.
3. **`chatId` rides the entry.** A writer must name the chat it is writing to; nothing reads "the
   chat on screen" to decide where an entry goes.

### Where it lives

The **store is the single copy** (`EchoJayWorkspace`, one append-only log per chat, flushed as now).
The processor keeps a cursor, not a copy: entries not yet acknowledged by the store are held in the
same log with the same `seq`, so "the store has not caught up" stops being a reconciliation problem
and becomes a flush delay. The editor holds no conversation state - it renders the log and the
model's request body is composed from the same log, in `seq` order (one source for what he reads and
what B is told, which the current merge has to keep in step by hand).

`openChat` becomes: read the log, sort by `seq`, render. **The merge is deleted** (~130 lines).

### His five cases

- **Two windows open.** Both render the same log; an append is a store write plus a change
  notification. `seq` is allocated by the store (one allocator per chat), so two windows cannot mint
  the same number. Last-writer-wins on an *entry's payload* is safe because only one author ever
  writes a given entry: the run that owns it, or the window that created the user row.
- **A run surviving a window close.** The run's entry already exists, with its key as `id`. The
  processor's `EJAgentHome` keeps updating that entry by id while no window exists - which is what it
  does today, minus the snapshot-copy dance. A new window adopts nothing: it renders the log, and the
  live run's entry is simply the live run's entry. `fromRecord` becomes a question with a real
  answer - *is the client holding the run whose key is this entry's id?* - instead of an integer
  comparison that a replay can invalidate.
- **A run stopped from another chat.** The stop writes its line to the entry whose `id` is the run's
  key, in that entry's own `chatId`. There is no "current chat" in the path at all, which is the
  11:25 ruling made structural rather than remembered.
- **Migration.** Existing chats have rows with no `seq` and no `id`. On first load of a chat: assign
  `seq` in the order the file already has (the file order IS the historical truth), mint an `id` per
  row (the run key where a snapshot carries one, else a hash of kind+content+index), stamp `chatId`,
  write the chat back once, and log one line per migrated chat. A row whose snapshot carries a
  `runKey` keeps it as its id, so a run that was live across the upgrade is still found.
- **Chips.** They are part of the entry's `payload` (they already serialise; `toJson`/`fromJson`
  carry `chips`, `chipNotes`, `chipHandSet`). What changes is that **nothing re-derives them**: the
  live run writes them into its own entry when the ask arrives, and the renderer draws what the entry
  holds. `makeRecord()` stops being a transformation applied at render time to a view it cannot
  identify; "this run is over" is a field on the entry, written when it ends.

### What this removes, in behaviour

- a row cannot be in the wrong place, because its place is stored;
- a card cannot be in the wrong chat, because its chat is stored;
- chips cannot be lost by a re-identification, because there is no re-identification;
- the model's history and the user's transcript cannot diverge, because they are one list.

## 3. Estimate

In this rhythm (write, QUICK build, his retest), measured in work rather than wall clock:

| stage | what | size |
|---|---|---|
| A | the entry type, the store's log + `seq` allocator, migration, `chat_log_guard` with the five cases | ~1 build's worth of writing, the biggest single piece |
| B | the writers: every append site stamps chatId/seq/id (15-25 call sites, mechanical but wide) | ~half a build |
| C | the reader: `openChat` renders the log; **delete the merge** and its three rebuilt containers | ~half a build, and it is where the risk is |
| D | the agent row: update-by-id, `fromRecord` from the entry, drop `agentRunMsgId_`/`agentRunIdSeq_` | ~half a build |
| E | the model body composed from the log; the rig row (reopen with a run waiting) | ~half a build |

So **three builds of writing, two of them touching code that draws his chat**, plus one rig row. I
would not try to ship A-E in one build: A+B are additive (both logs exist, the new one is written and
not yet read), then C+D switch the reader over in one commit with the merge deleted in the same
commit, then E.

**Risk, stated plainly:** C is the one that can lose a transcript. Mitigation is that A+B make the
new log a *second* writer first, so a build exists where the new log can be compared against the old
one on his real chats before anything reads it - and a guard leg that replays his own 14:15 sequence
against both.

## 4. Before or after the shadow

**Before.** Four of today's reports are this one cause, and I have now spent parts of five rounds
patching its symptoms one at a time - each fix true, each in one more place. The shadow adds cards
and notices of its own (stage reports, a refusal line per slot) onto the same structure, so building
it first means writing more entries into a log that cannot hold their order. The shadow is also the
bigger and more interesting change: it deserves a chat that does not need re-testing underneath it.

Two things I would still ship in the meantime, because they are small and provable:

1. **The chips/controls fix now** (his 14:15 item 2): identify the live row by the run's KEY, not by
   the per-window integer the replay reissues. One condition in `agentRunViewFor`, and it restores
   the chips, Stop, Undo and Undo all on a reopened live run.
2. Nothing else in the chat. The order fault is the merge's, and the plan deletes the merge; a third
   patch to the cursor would be work thrown away.

---

## Queued into this round by later rulings (21 Oct)

1. **The status line under "Thinking…"** (his 15:28). Drawn from the entry's `payload.statusText`, written
   by the live run and cleared by the next step, an ask, or the end of the run. The frame's shape is
   agreed with B in `docs/CONTRACT_status_frames_2026-10-21.md` (SSE `event: status`,
   `{type,text,round?}`).
2. **The "your answer?" label's layout** (his 17:3x item 1). *"The label takes a whole column, so in a
   narrow window the step text is squeezed into a thin strip on the left. Put that label on its own
   line, or drop it when the window is narrow; the step text must always use the card's width."*
   The row's geometry is `EJAgentRunView`'s (`layoutRows`, the right-hand `right` column), and it is
   the same code the new entry will render, so it is done here rather than twice: below a width
   threshold the right word moves to its own line under the step, and the step text takes the full
   width at every width.
3. **The order fault, as a leg** (his 17:3x item 2). `chat_log_guard` must replay his two real
   sequences, not a tidy one:
   - **his 14:15**: a chat holding TWO identical "add a compressor" rows, a run started by the
     SECOND one, the window closed and reopened - the card must land after its own user message.
     This is the case the old cursor got wrong, and the leg asserts the ORDER, not just the presence.
   - **his 17:3x**: the user's message drawn AFTER the card once he had checked the Link, i.e. a row
     created while the view was elsewhere - the entry's `seq` is allocated when it is created, so a
     later-created row cannot sort before an earlier one whatever the view was doing.
   Both are order assertions over a stored log; neither can pass by accident, because a `seq`-sorted
   render has no cursor to be lucky with.

## Also queued into this round: companion calibration loops on a Link (his 17:5x)

> *"Please build the companion loops so several compressors dialled on one Link each get their own
> loop, as the V2's own rack does. Sean's vocal chains on Links have two or three."*

The V2 has had this since 30 Sep (`calibStartMany` → `calibExtra_[uid]`, "ONE HOLD PER COMPRESSOR").
A Link has **one** `calibLoop_`, so the second dial of a build replaces the first. Three pieces, in
this order, and the middle one is the only risky edit:

1. **`calibStartMany` on the Link.** The chain-cmd road already collects every dial in the batch
   (`dialsOnPlaybackFromOps`, shipped today). It builds one `Config` per dial, each resolved to its
   own slot by identity, and hands the list over at once - the same reason the V2's version takes a
   list: starting them one at a time would have each replace the last.
2. **The tick services every loop.** `calibTickOwnRack` is ~150 lines keyed on `calibLoop_`: the
   staleness check, the 3 s window, the two sensors, the judgement, the sidecar write. The body is
   extracted to `tickOneCalib (CalibLoop&)` and called for the primary and then for each companion.
   Mechanical, but it is the Link's measurement path, so it ships with `calib_link_guard` (the
   script-based ctest entry, not a quick.sh target) run before and after on the same fixture.
3. **The sidecar carries more than one.** `rc.calib` is a single var and the V2's card renders from
   it. A `calib_extra` array beside it, absent when empty, so an older V2 reads the primary exactly
   as it does today and a new one can show all three. Additive, by the same rule as every other
   sidecar field.

Until it lands, a Link dialling two compressors starts the first and **says so** rather than
dropping the rest silently - which is the half that is already true, because the collector logs each
dial it resolved.

---

## The 19:51 recurrence, and what it tells stage C to build (his instruction: no patch to the old path)

> *"The Aitch_4_01 run was waiting on "How's it sounding?"; Sean opened a NEW chat on the mix bus and
> typed "build a mix bus". The Aitch run's "Stopped. 6 changes" card appeared at the top of the new
> mix bus chat."*

**The 11:25 fix fired and was right.** From the 19:51 log:

```
19:52:13  EJAgentRow: recording this run's line in ITS OWN chat 1792608277450, which is not the one
          on screen ()
19:52:13  EJAgentRow: opened run row #3 key=a9c4b96b... from=a fresh seed
```

The *line* went to the Aitch chat. A **row was then born** in the window whose current chat was `""` -
the new chat did not exist yet. The birth gate is `rowBelongsInChat`, and its documented rule is
*"an unknown either side means DRAW IT"*, written because an invisible live run he cannot stop is the
worse failure. That rule is right for a window with no chat open and wrong for a window that is
about to become a **different** chat, and it cannot tell the two apart, because a new chat has no id
until it is created. So this is not a third instance of one bug: it is the **row-birth step itself**,
which is what the stored log removes.

**What stage C must therefore build, stated as a rule rather than a patch:**

- the view renders the entries **of the chat it is showing**, selected by `chatId`, sorted by `seq`;
- **nothing creates a row as a side effect of rendering.** A run's entry exists from the moment the
  run starts, in its own chat; a window that opens on another chat simply does not select it;
- a live run updates **its own entry by `id`** - the run's key - and a window with no chat open yet
  has nothing to capture, because capture is not a step anything performs any more;
- `rowBelongsInChat` and `agentRunMsgId_`/`agentRunIdSeq_` go with the merge in C. They are the two
  proxies this fault family lived in.

**Legged now, in `tools/chat_log_guard`** (ST1-ST6 and OR1-OR4, green): his 19:51 sequence - a run
waiting in one chat, a new chat typed in, the stop written by id - asserts that the new chat holds
**no entry of that run** and renders only what was typed in it, while the Aitch chat carries the
stopped card in its own place; and ST6 asserts the other direction, that a run's own chat still shows
it, which is the rule the birth gate was protecting. The guard's `renderOrder` IS stage C's reader,
written here first so the editor's version has something to be compared against. **No patch was made
to the old path**, per his instruction.
