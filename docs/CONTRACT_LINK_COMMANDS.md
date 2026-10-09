# Link command contract

**Status:** proposed, 9 October 2026. Written before the code, as ruled, so that A2's UI, B's agent "do" tools and
this plugin all name the same things. **Nothing in here is built yet.** Where a shape already exists in the tree it
is marked **EXISTS** and quoted from the source; where it is new it is marked **NEW** and the open question is
stated rather than hidden.

The subject is the gap §2 of `LINK_REMOTE_CONTROL_PLAN.md` names: the Link is the only process that sees the
audio, so every rack operation the V2 offers has to become a command the Link executes. This document is the
vocabulary, the acks, the sequencing, the undo records and the latency targets for that.

---

## 1. Three channels, three jobs

| channel | transport | carries | acked |
|---|---|---|---|
| **value ring** | shared memory, one ring per rack, drained in `processBlock` | continuous values: a knob drag, a fader, a trim, a wet | implicit (see §5) |
| **chain channel** | `chain-cmd-<uid>.json` → `chain-ack-<uid>.json` | structure: add / remove / move / replace, and dialled settings | explicit, with a staleness guard |
| **control channel** | `ctrl-cmd-<uid>.json` → `ctrl-ack-<uid>.json` | the Link itself: active, gain, placement, level-record reset | explicit, per-seq |

The last two **EXIST** (`EchoJayProcessor::writeChainEditCommand`, `LinkProcessor`'s ctrl poll) and are kept. Only
the ring is **NEW**. The rule that decides which channel a command belongs on is a single question:

> **Does the user expect to hear it while their finger is still moving?**

Yes → the ring. No → a file. A drag has no meaningful ack (the next frame supersedes it); an `add` has nothing but
its ack (a plugin either loaded or did not). Building one channel for both is how the current design ended up with
a 3.5 s arm floor on a fader.

---

## 2. The commands

Every command names its channel, and the name is the wire name — B's tool layer and A2's UI use these strings
verbatim. Slot indices are **original numbering** throughout, as `ChainEditOp` already is, and `uid` is the Link's
instance uid (the registry key, the same string the sidecar and the mixer strip use).

### 2.1 Value commands — the ring (NEW)

| command | fields | meaning |
|---|---|---|
| `set_param` | `slot`, `param` (schema id), `value` (double) | one parameter on one slot's device, in the device's own units. **The HUMAN GESTURE path only** - a knob being dragged. An agent's considered change is `set` on the chain channel: see §8 |
| `slot_in` | `slot`, `db` | the slot's pre-trim (`setSlotPreTrimDb`) — **MISSING as an op today**, §2 of the plan |
| `slot_out` | `slot`, `db` | the slot's output gain (`setSlotOutGainDb`) — likewise missing |
| `slot_wet` | `slot`, `pct` (0..100) | the slot's wet/dry |
| `master_wet` | `pct` (0..100) | the rack's master wet — **local only today** |
| `pre_gain` | `db` | the Link's pre-chain gain |
| `link_gain` | `db` | the Link's own output gain. **Also on the control channel as `gainDb`** (EXISTS); the ring is for the drag, the file for a settled absolute value (§6) |

### 2.2 Structural commands — the chain channel (EXISTS, two additions)

`add`, `remove`, `replace`, `move`, `bypass`, `set` (with `settings_structured`), `set_wet`, `reset_levels`,
`level_match`, `headroom`, `undo` — the vocabulary in `ChainHost.cpp` today, unchanged.

Two **NEW** members:

| command | fields | meaning |
|---|---|---|
| `open_editor` | `slot`, `where` = `"float"` \| `"embed"` | the Link opens the slot's plugin editor. `float` is the default and the only one promised; `embed` is refused unless the same-pid proof in §4 of the plan holds at runtime, and the refusal says so |
| `close_editor` | `slot` | symmetric, so a UI that opened a window can close it without guessing |

`bypass` and `set` stay on the chain channel even though they are instant, because both change what the rack *is*:
a bypass is a decision with an undo step, and a `set` carries a whole `settings_structured` block. A momentary
bypass **button-down/up** gesture is not a use case anyone asked for; if it becomes one it gets a ring command and
this line gets deleted.

### 2.3 Which channel a mixed gesture uses

A drag that ends is **ring frames, then nothing**. The ring's `end` frame is the settled value and the authority;
no file command follows it. The exception is `link_gain`, where the control channel's absolute set already exists
and older Links understand only that — so during the build both are sent and the Link takes whichever arrives,
idempotently. **That dual send is a dev-switch path and comes out before beta.**

---

## 3. The ring, precisely

**NEW.** Modelled on `LinkShm`'s registry: a mapped block with plain `uint32_t` members so the byte layout is
identical on both platforms, atomics overlaid rather than declared (`LinkShm.h`'s own note on why), and a
`shm_layout_guard` leg asserting the offsets byte-for-byte before anything else is believed.

```
CmdRing                       (one per rack uid)
  uint32 magic                 0xEC4A3003
  uint32 layoutVersion         1
  uint32 capacity              256            // power of two
  uint32 writeIdx              // producer only
  uint32 readIdx               // consumer only
  uint32 overflowCount         // producer increments when it laps the reader
  CmdFrame frames[capacity]

CmdFrame                      (64 bytes, alignas(64))
  uint32 seq                   // strictly increasing, LinkShm::nextCtrlSeq's discipline
  uint32 kind                  // the command, as an enum, NOT a string
  uint32 phase                 // 0 begin | 1 stream | 2 end
  uint32 gesture               // gesture id: every frame of one drag shares it
  int32  slot                  // -1 where the command has no slot
  uint32 paramId               // interned schema id (§3.2)
  double value
  uint32 flags
  uint32 reserved[3]
```

**Coalescing.** The consumer drains everything available each block and keeps, per `(kind, slot, paramId)`, **only
the newest frame** — except that a frame with `phase == begin` or `phase == end` is never discarded, because the
gesture boundaries are what make one undo step per gesture possible (§6). A drag that writes 400 frames between
two buffers costs the audio thread one write per distinct control, not 400.

**Overflow is reported, not hidden.** If the producer laps the reader it increments `overflowCount` and the
consumer logs the count once per gesture. A dropped intermediate frame is harmless by construction (the newest
wins); a dropped `end` frame is not, so `end` frames are also re-sent on the control channel as a settled absolute
value when `overflowCount` moved during that gesture. **That is the one place the two channels overlap by design.**

**3.2 `paramId`.** Strings do not go in the ring. Each rack's slot publishes its schema ids in the sidecar it
already writes, in order; `paramId` is the index into that list. A V2 holding a stale sidecar therefore cannot
address a parameter the Link does not have — it can only address the wrong one, so the frame also carries the
slot's `structureRevision` in `flags`' low 16 bits and a mismatch is **discarded with a count**, never applied.
This is the same class of mistake `baseSlots` guards on the chain channel, and it gets the same treatment.

---

## 4. Acks and errors

### 4.1 Chain channel (EXISTS)

`chain-ack-<uid>.json`, written by `LinkProcessor::writeChainAck`:

```json
{ "v": 1, "seq": 12345, "status": "ok" | "stale" | "refused",
  "perPluginResults": [ ... ], "perPluginDetail": [ ... ], "reason": "in words" }
```

`stale` means `baseSlots` did not match the rack the Link holds — the V2 re-reads and re-sends. `refused` carries
`reason` and is terminal: the caller shows the words, it does not retry. **Unchanged, and the agent's `do` tools
surface `reason` verbatim rather than paraphrasing it.**

### 4.2 Control channel (EXISTS)

`ctrl-ack-<uid>.json` and, since v9, `ctrl-ack-<uid>-<seq>.json`. Field-presence commands: a field absent is a
field not touched, which is why a hand-built gain-only command cannot deactivate a Link.

### 4.3 Ring (NEW)

**There is no per-frame ack and there must not be one** — an ack per drag frame is the latency problem again.
Instead the Link publishes, in the rack's existing meter frame, the last applied `(gesture, seq)` and a
`cmdDiscarded` count. The V2 reads that at its display rate and that is the whole feedback path:

- the value it sent is reflected in the readouts it already polls (item 5's IN/OUT, the wet knob, the Level card);
- a gesture whose `end` seq never appears as applied within **250 ms** raises one line, once, and the V2 re-sends
  the settled value on the control channel;
- `cmdDiscarded` moving means a revision mismatch (§3.2) — the V2 refreshes the rack model rather than retrying.

**An error on the ring is always "nothing happened", never "half happened".** A frame either applies in full on one
buffer or is discarded. There are no multi-frame ring commands.

---

## 5. Sequencing

1. **`seq` is global per sending process** and comes from one author, `LinkShm::nextCtrlSeq()` (EXISTS: seeded at
   wall seconds, strictly increasing, so two commands in the same millisecond still differ — that bug has been
   fixed once already and is not being re-introduced on a new channel).
2. **A Link refuses a `seq` it has already applied**, per channel. EXISTS on both file channels
   (`lastAppliedCtrlSeq_`, the chain path's duplicate check).
3. **Ring frames are ordered by `seq` within a gesture** and the consumer applies them in that order. Across
   gestures there is no ordering promise and none is needed — two simultaneous drags are two independent controls.
4. **Structure and values do not race.** While a chain-channel command for a rack is in flight (sent, not yet
   acked), the V2 **stops sending ring frames for that rack** and resumes on the ack. A `move` that renumbers
   slots while a drag addresses slot 3 by index is the exact failure this closes; the revision check in §3.2 is
   the belt to this braces.
5. **One writer per rack.** The ring is single-producer/single-consumer. The rack lock that already exists
   (`kRackLockRenewMs` / `kRackLockExpireMs`) decides which V2 is the producer; a second V2 does not write.

---

## 6. Undo records

The design is §9 of the plan, made concrete. **Every command in §2 is undoable, cross-process, and the record
lives in the sending V2's plugin-wide history** — the same `CHAIN` class the local rack uses, so one Undo button
walks local and remote edits in one order.

**One step per gesture, pushed on `end`:**

```
{ uid, what, slot, param, before, after, gesture, seq }
```

- `before` is captured on `begin`, **from the Link's published value**, not from the V2's own widget — the widget
  may have been dragged from a stale position, and an undo that restores the stale value is worse than none.
- `after` is the `end` frame's value.
- A gesture whose `before == after` **pushes nothing**. A drag that returns to where it started is not an edit.
- A structural command pushes one step per `applyChainEdits` batch, which EXISTS, and the batch's ack is what
  confirms it.

**Undo sends the prior value as an ordinary command and waits for its ack.** On the ring that means a synthetic
one-frame gesture (`begin`+`end`, same `gesture` id, `flags` marked `isUndo`) and the 250 ms applied-seq check of
§4.3; on a file channel it is an ordinary command with an ordinary ack. **An undo that did not land says so** and
leaves the stack untouched, rather than popping a step the rack never took. That is the one case where the V2
reports a failure to the user instead of correcting itself.

**Redo is the same record played the other way** and inherits all of the above.

**Not Logic's header Undo**, for the reason in §9 of the plan: a Link Mixer move is not a host parameter on the
V2, and making it one puts every Link's gain into the V2's parameter list.

---

## 7. Latency targets

Sean's standing rule is that a timing is not evidence until its clock is stated, so each of these names what is
measured and where from.

| path | target | measured as |
|---|---|---|
| ring frame written → applied in the Link's `processBlock` | **≤ 1 buffer** (2.7 ms at 128 samples, 10.7 ms at 512) | the Link logs the wall-clock delta between the frame's write stamp and the block that applied it; the leg asserts the bound at both buffer sizes |
| drag → audible | **≤ 30 ms** | the above plus the host's own output latency, which is not ours; the leg asserts our half |
| structural command → ack | **≤ 500 ms** typical, **5 s** budget before the caller gives up | EXISTS as a budget ("the 10 s ack wait stays at 5 s"); the **typical** figure is NEW and currently unmeasured |
| `open_editor` → window on screen | **no target yet** | third-party editors open at their own pace; the command acks when the Link has *asked*, and a separate state says whether the window is up |

**What I have not established:** the ≤ 1 buffer figure is arithmetic from the design, not a stopwatch. §10 of the
plan says the same about the file channel. Both get measured in stage 1 and this table gets the real numbers.

---

## 8. Naming — RECONCILED WITH B, 9 October (evening)

B's `docs/CONTRACT_AGENT_TOOLS.md` now exists and was revised against this file. **Sean's rule: where a name
differs, B's is the wire and I rename.** B resolved it better than a rename: the **agent `do` op** is the wire name
the model uses, and the ring / chain command it maps to **stays internal to the plugin**. So the names below are
unchanged inside the plugin, and B's column is what the model and the server say.

| agent `do` op (B's wire) | this file's command | note |
|---|---|---|
| `set` | `set` (chain, `settings_structured`) | device units; **several params in one `set` is still ONE command and one ack** |
| `set_wet` | `slot_wet` (ring) | |
| `set_io` | `slot_in` + `slot_out` (ring) | one op may carry **both** `inDb` and `outDb`; two ring frames, one ack |
| `set_master_wet` | `master_wet` (ring) | |
| `set_pre_gain` | `pre_gain` (ring) | |
| `bypass` | `bypass` (chain) | |
| `add` / `remove` / `replace` / `move` / `build` | the same (chain) | `build` = the chain block, one ack, one undo step |
| `set_level` | `set` on the EchoJay Level slot | the level_params contract (08c item F2) |
| `open_editor` | `open_editor` (chain, `where:"float"`) | free on the agent side; the ack is the result |
| **not agent ops** | `link_gain`, `reset_levels`, `level_match`, `headroom`, `undo`, `close_editor` | the Level slot owns level; undo is the plugin's |

**`set_param` is corrected.** §2.1 listed it on the ring as the general parameter path. It is not an agent op: an
agent's considered change is `set` on the **chain** channel, with its ack and its staleness guard. The ring's
`set_param` is the **human gesture** path — a knob the user is dragging — and that is the only thing it is for.
One name for two jobs was the mistake; they are now two.

**B adopted the ring's ack rule verbatim** (§4.3): no per-frame ack, the plugin answers the agent's call when the
meter frame shows the settled `(gesture, seq)` applied, else `{ok:false, error:{code:"not_applied", message:
<sentence>}}` after the 250 ms rule. A refusal's `reason` reaches the model as a sentence, unparaphrased.

### My three open questions, answered by B
1. **Batch or one command?** One `do` = one command = one ack = **one undo step**. A round may carry several; the
   plugin runs them in order. That is what §5 and §6 already assume, so nothing changes here.
2. **Device units or normalised?** **Device units only, never normalised** — `params` in the built-in's own units,
   `controls` in the map's display units as the registry prints them. The plugin maps display to normalised itself.
   This is what I assumed, and for the reason I gave: normalised values are what the fingerprint/map era got wrong.
3. **Who waits for playback?** **The plugin owns the wait**, as `check(playback)`: answered when `min_seconds` of
   gated audio has played, or after **60 s** with `{played:false, waited:60, sentence:"Nothing played in 60
   seconds - play the loudest section and ask me again."}`. The server's `await` watchdog is 90 s so it outlives
   ours. We do **not** wait twice. A2 has already built this client-side; the executor supplies the reading.

### Two things B's contract settles that are mine to honour
- **The agent card and the ask shelf are never shown together** (Sean). An ask on an agent turn is rendered as the
  card's own buttons, and the server emits no `<<<ECHOJAY_ASK>>>` block on an agent session. That decides A2's
  open question E3 the way A2 recommended, and it is the way I would argue for independently: the ask *is* the
  card, so a shelf underneath is a second question for one decision.
- **E's gap is closed on the server side.** B's validator `ceiling_fixed` refuses a final limiter's ceiling that is
  not -0.1 dBTP before it is ever sent, with `resend: {ceiling_db:-0.1}` — so the third-party final limiter case
  my 08c item E could not reach from the built-in dial funnel is covered where it belongs. Both halves now exist.

### Still owed, for B
- **`CONTRACT_LEVEL_PARAMS.md` is referenced twice and does not exist in the saas repo.** The plugin half is built
  and gated (08c item F2): option `"match" | "pushed" | "dynamic" | "commercial" | "keep"`, `"match"` valid with
  **no** `target_lufs`, stored as option 4 on the EchoJay Level device, GR cap 3 dB, and only FullMix and
  MasterBus hit a target (Sean, 9 Oct) — every other channel and bus **volume-matches**. Targets are integrated
  LUFS. That is what the file should say; I have not written it, because it is B's.

---

## 9. Naming, for B (the original offer, kept for the record)

### What I proposed before B answered

These are the strings I have chosen for the wire. B's `docs/CONTRACT_AGENT_TOOLS.md` does not exist in this tree
yet, so **where we disagree, B's names win and I rename** — the plugin is the one that can change a private wire
without a deploy.

- The eight commands B's `do` tools need are `set_param`, `slot_in`, `slot_out`, `slot_wet`, `master_wet`,
  `bypass`, `link_gain`, `open_editor`, plus the four structural ones `add`, `remove`, `move`, `replace`.
- `slot` is always **1-based in anything a user or a model sees** and 0-based nowhere in this contract's JSON.
  The existing `ChainEditOp.slot` is the original index as the model numbered it; the ring keeps the same
  convention so there is one rule.
- A refusal's `reason` is a **sentence**, because it is shown to the user and read by the model.

**Open questions for B:**
1. Does an agent `do` call want to be one command or a batch? A batch needs one ack for the lot and one undo step
   for the lot; I have assumed **one command, one ack, one step**, and a batch is a client-side loop.
2. Should `set_param` accept a normalised 0..1 value as well as device units? I have assumed **device units only**
   — normalised values were what the fingerprint/map era got wrong.
3. Who owns "wait until the user plays"? I have `wait_for_playback(min_seconds)` on the plugin side from the
   `AudioPlayHead` (programme item 3); if B's loop also waits, we will wait twice.
