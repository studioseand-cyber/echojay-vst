# Multi-track jobs — "add the same vocal chain to all the vocal channels"

**IN THIS ROUND** (Sean changed the order, 14 Oct): after item 0 + 0b + Auto mode, **before** 0a. B starts
their half now; mine starts once `ship_2026-10-14a` is installed, not during the gate.
Read with `docs/DESIGN_AGENT_IN_CHAT.md` (the run is one message) and
`docs/MOCKUP_AGENT_IN_CHAT_APPROVED.md` (which wins on anything visual).

**Round order:** the login text fix → item 0 + 0b + Auto → **multi-track jobs** → 0a. Loop on the rig until
every row is green, then one full gate.

**Why it has to follow item 0 and not run beside it:** every visible part of this feature is a part of the
new agent message — per-track lines with their own read-back ticks, one batch approval, Undo all across
racks, and the tick-box pick, which is a **new multi-select ask payload** in that message. Built against
the floating panel it would be built twice.

**BOTH OPEN QUESTIONS NOW RULED (Sean, 14 Oct):**

- **Q1 — agreed:** only the three truthful cases in §1, and **never imply we scanned the project.** No
  sentence may suggest the plugin looked at the host's track list, because it cannot.
- **Q4 — ask ONCE for the whole batch**, as a choice rather than a confirmation, in these words:
  > *"N of these already have plugins. Replace their chains, or add after what's there?"*

  **Both answers are supported**, with rig rows for each (§4, M8 and M9).

---

## 1. Most of this already exists, and one part of it cannot be built as written

### Groups are already a ruled feature, not something to invent

`EchoJayProcessor::LinkGroup` — **a name, member uids in display order, an optional bus, a level offset, and
each member's last known name** — is persisted in the V2 session state as `linkGroups`, and **already goes
to the server** on every chat / chat-stream / classify body as `groups[]{id,name,members,bus}` beside
`links[]`. The API is there:

```cpp
juce::String createLinkGroup (name, members, bus = {}, juce::StringArray* refusedOut = nullptr);
static constexpr int kMaxGroupMembers = 64;   // raised from 16 on 28 Sep, because
static constexpr int kMaxGroups       = 16;   // "every channel" is the group that matters
```

and the 28 Sep ruling on `refusedOut` is exactly what a multi-track job needs: *"a group holds exactly the
selection, and anything it cannot hold is said out loud rather than dropped."* A 22-strip selection silently
becoming a group of 16 is the fault that ruling came from — the same fault a multi-track job would commit if
it quietly did 14 of 17 tracks.

`memberNames` carries each member's **last known name**, which is how a group already says *"&lt;name&gt; —
not in this session"* for a member whose Link is gone. **That line is the honest version of Sean's
missing-Link sentence**, and it already exists.

So "offer to save them as a named group" is a thin tool over a ruled API, and a named group used directly is
`linkGroups()` plus a lookup. Good.

### The part that cannot be built as written: discovering tracks that have no Link

Sean's bullet:

> *"Tracks without an EchoJay Link are named plainly ('BV 3 has no EchoJay Link; add one and I'll include
> it')."*

**A plugin cannot enumerate the host's tracks.** The only thing the host tells a plugin about tracks is its
*own* (`updateTrackProperties` → `props.name`, and `PluginProcessor.cpp:1717` takes exactly that, this
instance's track name). There is no AU or VST3 call that lists the other channels in the project. So the
plugin knows **only**:

1. its own track's name,
2. every **Link** that has registered in the shared registry (name, uid, connected, placement, role),
3. **group members**, including ones whose Link is not in this session (via `memberNames`).

It therefore cannot know that a track called "BV 3" exists at all, unless it has a Link, or it is in a saved
group, or the user says the words.

**What is honestly buildable**, and I would build all three:

| case | the line |
|---|---|
| a **group member** whose Link is not live | *"BV 3 is in this group but isn't publishing an EchoJay Link in this session — add one and I'll include it."* (the group store already knows the name) |
| the **user named** a track we cannot see | *"I can't see a track called BV 3 — I only see channels that have an EchoJay Link. Add one there and ask me again."* |
| the job ran and the **count is short of what was asked** | *"That's 6 of the 8 vocal channels. Hat 2 and BV 3 have no EchoJay Link."* — said **before** Apply, not after |

**What I will not build** is a sentence that implies we looked at the project and found a track without a
Link. That is the shape of claim this round has spent itself removing. **This is a question for Sean** (§6
Q1): the feature is the same in practice, but the words have to be about what we can see.

## 2. Target selection — the three routes Sean named

```
   "add the same vocal chain to all the vocal channels"
                │
      ┌─────────┴──────────┬────────────────────────┐
  a named group        clear names              unclear
   exists              match                     names
      │                    │                        │
  use it directly   list what was found,      tick boxes in the
  (no question)     ask ONCE to confirm       message, user picks
      │                    │                        │
      └──────────┬─────────┴────────────────────────┘
                 │
      offer "Save these as a group?" (one tap, names it)
                 │
            the batch runs
```

- **a named group** is used directly, with no confirmation step — the user already made that decision once.
  The message's opening line still says which group and how many members, because a group can have changed
  since it was made.
- **clear names**: the agent lists what it matched and asks **once**. One question, not one per track —
  Sean's standing complaint about loops.
- **unclear**: tick boxes **in the message**. This is the one new piece of UI: `TalkAsk` today is
  single-choice chips (`labels`, `details`, `intents`, `allowFreeText`), so a **multi-select ask** is a new
  payload kind — a step row with checkboxes, a count, and one Confirm. It renders in the transcript like
  every other card (§1 of the chat design), so nothing leaves the chat.
- **after any of the three**, one tap offers to save the selection as a named group, through
  `createLinkGroup` — and `refusedOut` is surfaced, never swallowed.

**Agent off:** the reply says plainly that this is a multi-track job and carries a **one-tap "Turn on
Agent"** button, which sets the chip to **Ask** (not Auto — a first multi-track job is not the moment to
discover Auto). The button is the same inline-button idiom as the mockup's chips.

## 3. Execution — one message, one line per track

```
┌────────────────────────────────────────────────────────────┐
│ EchoJay                              [ASK]      Working   │
│ Adding your vocal chain to the 6 channels in "Vox".        │
│                                                            │
│  ✓  Lead Vox          EQ → Comp → De-ess        applied    │
│  ✓  Double L          EQ → Comp → De-ess        applied    │
│  ◐  Double R          building…                 working    │
│  ·  BV 1                                        pending    │
│  ·  BV 2                                        pending    │
│  ⊘  BV 3    no EchoJay Link in this session     skipped    │
│                                                            │
│  ⏹ Stop                                                    │
└────────────────────────────────────────────────────────────┘
```

Rules, each one inherited from something already settled:

- **a line ticks only on read-back from that Link's own sidecar** — the rule this round put in nine sinks,
  and the rig already proves it per-Link in rows L1/L2 (*"that Link's own sidecar holds 2 slot(s)"*).
- **one approval for the whole batch in Ask**, none in Auto. Per-track approvals on seventeen tracks would
  be the single-Apply bug (item 2) multiplied by seventeen.
- **the batch is serial, not parallel.** Each Link's build is a chain command with an ack and a read-back
  poll (up to 20 s each); firing seventeen at once would make one Link's failure unattributable, and the
  build road is a choke point on purpose.
- **a track that fails does not stop the rest** — it fails on its own line with its own sentence, and the
  closing line says *"5 of 6. Double R refused the build: &lt;reason&gt;."*
- **Undo all covers every track.** This is the one real mechanism question: the plugin-wide history takes
  one entry per rack (`kind: "chain"`, *"a rack by uid"*), so a six-track batch pushes six entries and
  "Undo all" must walk **exactly those six** — not six steps of whatever is on top. The run already tracks
  its own steps' undo tokens (`canUndoAll` / `undoAll`), so Undo all stays token-driven, and the group entry
  kind (`kind: "group"`, added 21r item 4) is the precedent for one user action spanning many Links.
- **Stop** marks the untouched tracks stopped, and the closing line says how many landed — the mockup's rule.

## 3b. Replace or append — and why append cannot use the build road

Sean's Q4 ruling needs one design decision stated, because the two answers are **two different transports**:

| answer | how it is sent | why |
|---|---|---|
| **replace** | the build road — `sendChainToLink` per Link (`loadChainFromJson(..., replaceConfirmed = true)` for the open rack) | a build **is** a replacement: the chain block is the whole rack. This is the path item 1 fixed and the rig already proves per-Link (L2) |
| **append** | **`add` edit ops**, one per slot, in order, each after the current last slot | **a build would destroy what is already there.** Appending by building "existing + new" would re-instantiate the existing plugins and lose their state — their parameter values, their own window state, everything the user has already set on those tracks. An `add` leaves the existing slots untouched because it never rewrites them |

So **append is not a build with a flag; it is N adds per track.** The chain channel's `add` already carries
`settings_structured` and `wet_pct` (and this round made the pass-through carry `wet_pct`, which had been
dropped), so the new slots arrive dialled either way.

Two consequences that have to be in the read-back, or the feature lies:

- **replace** asserts the target's slot count **equals** the chain length and the names match in order.
- **append** asserts the count is **before + chain length**, that the **original names are still present in
  their original order**, and that the new ones follow. A count alone would pass an append that had quietly
  replaced.
- a rack at the 15-slot ceiling (`kMaxChainSlots = 15`) cannot take the append: that track's line says so
  plainly and the rest of the batch continues — the same shape as a missing Link.

## 4. The rig rows Sean named, plus the two I would add

| row | what it proves |
|---|---|
| M1 | **a named group is used directly**: build to a 3-member group, no question asked, all three Links' sidecars read back the chain |
| M2 | **clear names confirmed once**: "all the vocal channels" with three Links named Vox/Vox 2/Vox 3 → one question, one answer, three builds |
| M3 | **the tick-box pick**: unclear names → a multi-select ask, the rig ticks two of four, exactly those two are built |
| M4 | **a missing Link is named and skipped**: a group with a dead member → that line reads "no EchoJay Link in this session", the others complete, and the closing line counts honestly |
| M5 | **Undo all across tracks**: after a 3-track batch, Undo all leaves all three sidecars as they were, read back from each |
| M6 | *(mine)* **the group save**: the offered save creates a group with exactly the picked members, and a 65th member is **refused out loud** (`refusedOut`), not dropped |
| M7 | *(mine)* **Agent off**: the one-tap button turns the chip to **Ask** and nothing was changed before the tap |
| M8 | **the batch asks once, answered REPLACE**: three Links, two already carrying a chain → one question in the words Sean ruled, answer "replace", and each Link's sidecar reads back **exactly** the new chain, in order |
| M9 | **the batch asks once, answered APPEND**: same setup, answer "add after" → each sidecar reads back **the original slots in their original order** followed by the new ones, and the count is before + N. A rack at 15 slots is named and skipped, and the others still complete |

## 5. What each side owns

**A (me):** the multi-select ask payload and its row; the batch runner (serial, per-Link read-back, partial
failure); Undo all across racks; the group-save tool over `createLinkGroup` with `refusedOut` surfaced; the
Agent-off button; rows M1–M7.

**B:** the rules for *which* tracks a phrase means (and the confidence that decides confirm-vs-tick-boxes);
the group-save tool call; one approval for a batch rather than per step; the words for a short count; and
sim rows. The group wire format already exists (`groups[]` beside `links[]`), so nothing new is needed to
*read* groups — only to **create** one.

## 6. Questions for Sean

1. **The missing-Link sentence.** We cannot see a track that has no Link (§1). *Recommendation: keep the
   feature, change the words* — name missing members of a **group** by their last known name, name a track
   **the user mentioned**, and otherwise say "6 of the channels I can see". Shall I proceed on that basis?
2. **Does a named group get a confirmation anyway?** *Recommendation: no question, but the opening line
   states the group and the member count* — so a group that has grown since it was made is visible without
   adding a step.
3. **How many tracks before the batch itself needs a confirmation in Auto?** *Recommendation: no special
   case under 8, and above that Auto states the count in the opening line and proceeds* — a limit Auto
   silently imposed would be Auto not doing what it says.
4. **Does "add the same chain" mean replace or append on a track that already has plugins?**
   *Recommendation: it must ask, once, for the whole batch* ("3 of these 6 already have a chain — replace or
   add after?"). A build replaces the rack (`loadChainFromJson(..., replaceConfirmed = true)`), so silently
   taking "the same chain" as "replace" would delete work on five tracks at once.
