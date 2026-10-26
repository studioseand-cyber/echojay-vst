# Overnight 25 -> 26 Oct: progress

Installed when the night began: V2 AU `A831AC56`, Link AU `B0BAC26E` (commit e697be8b).
Sean's rule tonight: nothing may depend on him being at the Mac; prove on non-PACE plugins.

| # | item | state |
|---|------|-------|
| 1 | live view smoothness (follow vs control, and the wobble) | **done**, in the 21:02 build (guards SH27/SH28 + bench `drag` leg) |
| 2 | the refusal count | **done** (tally at 10/100/1000 and when the refusal lifts) |
| 3 | the disappearing new-chat message | **hole found and fixed** (SH29); not a PROVEN cause - see notes |
| 4 | the EchoJay Limiter's ceiling not taking | **cause found and fixed** (SH30); bench owed |
| 5 | dial bounds from B's contract | **done** from `b/slot-dial`'s copy of the contract (guards 17j-17j8) |
| 6 | the runtime reduction/threshold check | **done**, building at 22:43 (SH31a-f) |
| 7 | dialling a slot already on the rack | **not done** - see why below |
| 8 | clip_drive with its dial | **not done** - see why below |
| 9 | read-only: which build Pro Tools loads, and the missing [pid] | **done** - see notes; nothing changed |
| 10 | 03:00 at the latest: full gate + rig | **done** - S1..S5 green, S6 RED on two live-model rows (not tonight's work) |
| 11 | install the full-tier build only on S4 100% + S5 green | **installed** - S4 100%, S5 green |
| 12 | the morning report, then A_NIGHT_DONE_26 | **done** |

## Notes as they happen

### 21:02 - items 1 to 5 written, build running

**Item 1 (a), follow fighting control.** Confirmed by construction, not by instrumentation:
`shadowApplyChangedValues` walked every parameter the frame carried and had no notion of a gesture
at all. So a drag went copy -> ring -> Link -> publish -> copy and the value that came back was the
Link's from one or two frames earlier - a write BACKWARDS into the knob he is holding, twenty times
a second. Fixed as he ruled it: `echojay::followMayWriteParam` holds a parameter from gesture begin
to end plus a 250 ms settle, per PARAMETER, so every other knob still follows on the same frame.
The echo test he mentions was already there (`lastAppliedFromLink`).

**Item 1 (b), the wobble.** Also by construction. `NativeClip2_attach` runs from the panel's timer
every 100 ms; its two frame writes were guarded by NSEqualRects, and seven other things were not -
`setBoundsOrigin`, `setWantsLayer`, `masksToBounds` and the four layer-style properties. Every CALayer
property assignment marks the layer as needing display, so the layer holding the hosted AU's own
view was recomposited ten times a second whether anything had changed or not. `echojay::clipWorkFor`
decides; a steady tick is now a no-op. This is why the wobble was there with no run going: the
shield fix of 25 Oct could not have touched it.

**Item 3.** NOT a proven cause. The 22 Oct log has rotated away and the 25 Oct rig run carries no
clear-site line, no editor rebuild and no drop line, so the two sightings may be different faults.
What reading the code DID find is a real hole: the 22 Oct fix gives a dropped run CARD a home before
the replay list is cleared ("a row that names its own chat has a home; it goes there, and only then
is the list cleared") and a row without `agentJson` got no such thing - his own typed words, in a
chat the store had not caught up with, were counted in "dropped N unsynced row(s)" and deleted. That
is wrong whatever caused the sightings, so it is fixed and guarded. The rig now also names which
window published each snapshot, which settles the rest on the next run.

**Item 4.** Cause found exactly. The final-limiter ceiling hold fires when the limiter
`slotIndex == slots_.size() - 1`, and that was evaluated DURING the build - so the first of two
limiters, last for the moment it was added, was held at -0.1 when B asked for -1. B's own note says
it could not show before the two-limiter shape of 24 Oct because every add until then asked for -0.1
anyway, which is the signature of a rule that was never about the asked value. The hold now runs
when the edit sequence has finished and the shape is settled, and a limiter that is no longer last
gets back the ceiling its block asked for.

**Item 5.** The contract was not on disk; it is on B's branches, read with `git show`
(`b/slot-dial:docs/CONTRACT_dial_bounds_2026-10-25.md`) without touching that repo. Implemented as
written, including rule 3 (one side of a pair serves its twin) and "never a guessed range" - two
ranges that do not overlap are treated as unknown rather than inverted, because the loop writes one
value to every named control. `bounds` beats `min_db`/`max_db` when both arrive, and the fact rides
the sidecar so a handover cannot put the dB default back.

### Item 9 (read only, nothing changed)

**Which build Pro Tools loads.** `/Library/Application Support/Avid/Audio/Plug-Ins/`:

| bundle | version | binary built | arm64 UUID |
|---|---|---|---|
| `EchoJay V2.aaxplugin` | **2.26.4** | **29 Sep 2026 10:10** | `AA0C2737-D815-39AD-815C-D92CBB1BE3BD` |
| `EchoJay Link.aaxplugin` | **0.8.5** | 29 Sep 2026 10:10 | `13D36AE3-CD76-36F9-B3F9-77D614C0B33D` |
| `EchoJay.aaxplugin` (root-owned) | 1.6.3 | 9 Jul 2026 | the old single plugin |

So **Pro Tools is four weeks behind**: 2.26.4 against the AU/VST3's 2.26.5 plus everything since.
`tools/install_local.sh` writes `~/Library/Audio/Plug-Ins/Components` and `.../VST3` only - it has
never touched the AAX path - so every install this month has gone to Logic and left Pro Tools where
it was. Any Pro Tools test is a test of the 29 September build.

**Why those log lines have no `[pid]`.** Not a mystery binary. The line itself is ours -
`LinkProcessor.cpp:3275`, `"EJLinkState: registry snapshot (" + why + ") uid=..."` - and the string
is present in the 29 Sep AAX binary. What is missing is the FILE LOGGER'S prefix: `[%d]` with
`getpid()` was added to `EchoJayFileLog.h` on **12 October** (commit `1a570bf5`). The AAX build
predates it by two weeks, so it writes the old format. Both builds share one rolling-log directory,
which is why a Pro Tools session and a Logic session interleave in one file and look like two
writers - and why the 2 MB rotations every eight minutes wiped the 17:25 evidence.

### 21:45 - the first verdicts

`chat_store_guard` **GREEN, 305 assertions** (286 last night): SH27a-d (follow vs gesture),
SH28a-d (the steady tick), SH29a-e (a dropped row's home), SH30a-d (the final limiter's ceiling)
all green. `comp_contract_guard` and `overlay_liveness_guard` green.

`level_loop_guard` RED 2, and **both are mine, not the product's**:

* **(6a) "a delay is not a dynamics slot by its OWN category"** - a PRECONDITION, failing because I
  pointed two runs of that guard at the same `ECHOJAY_STATE_HOME`. Leg (6a) calls `storeParamMaps`
  to mark Apple's AUDelay `category=compressor`, that lands in the state root's `param_maps.json`,
  and the next run loads it back and starts from a world where a delay IS a compressor. My own note
  on this (2 Oct) says each run needs its own root; my runner reused one. Fixed in the runner with a
  fresh root per guard, and the product is untouched.
* **(17j8)** - my new leg read the TRANSLATOR's reason for a sentence the PARSER writes. 17j6 is the
  leg that proves the translator passes `bounds` through untouched, so `configFromBlock` is where the
  range becomes the loop's and where the line about it belongs. Leg corrected to read `pwhyB`.

The bench's drag leg did not run: `REFUSED: this plugin reports no parameters`. I passed AUDelay by
a component PATH; the bench takes the format identifier, which `run_legs.sh` already knows
(`AudioUnit:Effects/aufx,dely,appl`). The invocation now goes through that runner.

### 22:42 - items 1 to 5 are green

| | |
|---|---|
| S4 | **85/85 on assertions.** The one non-zero was `level_loop_guard`, failing MY new leg (17j8) and nothing else; corrected and green since |
| S5 | **GREEN** both sides |
| `chat_store_guard` | **GREEN, 305** -> 311 with item 6's legs |
| the bench, `drag` on AUDelay | **3/3**: with the hold **0 backward steps in 40**, span 0.900; without it the drag DOES step backwards |

The bench is the one that matters for item 1: two live, rendering AUDelay instances, a scripted
40-step drag, a follow frame after every step carrying the Link's value from two publishes ago -
which is what 30 Hz publishing against a 20 Hz read amounts to. His "it jumps" is reproduced in the
known-bad half and absent in the product's.

### Items 7 and 8: not done, and why

Both are new agent operations, and both stand on **`dial_step`** (`CONTRACT_dial_step_2026-10-23.md`),
which is not built yet - it has been in the queue behind tonight's faults since the 25 Oct round.
Item 7's contract is explicit about it: *"Everything else is the dial step contract, unchanged"* -
the `op: "dial"` frame, `step_text` on the card, the running indicator, the 60 s wait for playback,
`skipped`/`not_heard`, and the `{ status, dial: { name, slot, outcome, figure_db, end, control,
value } }` answer. Item 7 then adds the `slot` variant and the `dial_slot` capability; item 8 adds
`clip_drive` and its own dial.

I am not starting them at 22:45 with the full gate due at 03:00. A half-landed agent op is worse
than none: the model would send frames the build answers wrongly, and the only honest proof of
either is a rig run, which needs the gate's slot. Both are written up and ready to start first
thing, in this order: `dial_step`, then item 7 (`dial_slot`), then item 8 (`clip_drive`).

What I did do for them tonight: **item 5 (the bounds) is a dependency of both** and is finished, so
when the dial op lands it will already clamp each control to its own range.

### 23:52 - a 35-minute S4 lost to a half-applied rule, and the rule restated

The S4 after item 6 came back 92%, SEVEN reds, all of them the compile-a-side family refusing:

    the Link archive is OLDER than the headers this would compile against
    archive: Oct 25 21:04:33 2026

Item 6 added a NEW header (`EJMapSuspect.h`) and touched the V2's executor only. `cmake --build
build-release --target EchoJay EchoJayLink` returned 0 and relinked **nothing** for the Link,
correctly - its own include graph had not changed. But the two-sided guards refuse an archive older
than **ANY** header, because they cannot know the include graph, so the Link's 21:04 archive was
refused against a 22:42 header it never includes.

**The rule is DELETE then build, not build.** My own note says exactly that and I applied half of
it twice in two days: yesterday with an `rm` that matched nothing (the filenames contain a space),
tonight by building without deleting. The gate's S1 does it properly, which is why the gate was
brought forward rather than re-running S4 in the quick tree.

### 23:59 - the gate started, and one honest note about the machine

It waited 20 minutes for the load to fall below 3.0 and then proceeded, as it is written to. The
load was **Sean's own machine**, not this work: `mediaanalysisd` at 68%, Spotlight's `mds_stores`
indexing tonight's ~70 new guard binaries, plus rekordbox, Adobe Creative Cloud, the UA Mixer
Engine and WhatsApp. Nothing of mine was runaway (I checked the full process list). It is worth
knowing when reading the gate's timings: anything that looks slow tonight may be contention rather
than the product.

### 03:10 - the gate's verdict

| stage | result |
|---|---|
| S1 forced delete + relink, three trees | **OK** (345-file manifest) |
| S2 the guard suite, LTO, universal | **OK** |
| S3 the four bundles | **OK** |
| S3b archive provenance | **OK** - the stage that exists for exactly the trap I fell into twice |
| **S4 `ctest -L fast|plugins`** | **100%, all 85** |
| **S5 `link_state_guard`** | **GREEN** both sides |
| S6 the rig, mock pass | **PASSED** (1213 s) |
| S6 the rig, live pass | **RED: 2 failed, 82 passed, 22 skipped** |

**The two rig failures are the live model's own road, not tonight's code.**

* **H2** *"(honesty) never names a plugin that is not on the rack"* - `THE CARD SAID NOTHING, there is
  no claim to judge, so this row proves nothing and fails`. The live model produced no card for that
  prompt. The row fails honestly rather than passing on an empty card.
* **H3** *"how much gain reduction is the compressor doing on …?"* - the build answered *"I have no
  fresh reading from that track yet - play the loudest section and I will measure it."* That is the
  `needs_playback` / `link_meters_stale` refusal working correctly; the rig has no transport running,
  so there is no reading to quote. The row wants a figure that cannot exist in this run.

Neither touches the live view, the refusal count, the chat home, the limiter ceiling, the dial
bounds or the map check. The 22 skips are the usual live-road ones (the model's pace, no playback,
no GR-reporting device on the rack).

### And the rig answered item 3's open question

The instrumentation I added last night reports which window published each snapshot, and it **killed
my own leading hypothesis**:

    his message WAS drawn in the new chat and was gone again by the snapshot these checks read
    (onScreen rows now: 0, user rows 0, cards 0). The window that drew it published as editor
    5392236544; the snapshot these checks read came from editor 5392236544 - the SAME window, so
    the row really did leave that window's list

So it is **not** two editors taking turns at one state file. The row genuinely left that window's
`chatMessages`, with **no clear-site line in the log** - and every `chatMessages.clear()` has been
instrumented since 11 August. That means the list is being emptied by a road that does not log, and
the next step is to find it: the assignment at `PluginEditor.cpp:20448` (`chatMessages = rows`) is
the only other mutation in the file and it is guarded by `if (added > 0 || stampedLate > 0)`, which
cannot produce zero rows. Worth instrumenting that assignment and re-running B6 alone.

Tonight's item-3 fix (a dropped plain row gets a home) stands on its own merits and is green, but
**it is not this fault** - the honest position, now evidenced rather than suspected.
