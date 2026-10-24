# FOLLOW layer 3 — the changes a plugin does not report

**Design only. No code.** Asked for overnight, 24→25 Oct 2026.

Layers 1 and 2 are in and proven: the copy opens on the Link's real settings (layer 1, passed at
20:40) and follows its parameters at 30 Hz (layer 2, passed at 21:31). This is the third kind of
change — the one neither of them can see.

## 1. What it is for

Some changes a plugin makes to itself report **no parameter at all**: a preset recall, a filter-graph
drag, an IR load, a modal dialog. After one of those, the Link's plugin and the V2's copy disagree
and nothing in layers 1 or 2 notices. The CL 1B is not such a plugin — all eight of its controls
report — so this is for the ones that are, and **none has been identified by name yet.** That matters
for the ordering: this is insurance, not a reported fault.

## 2. The mechanism, using fields that already exist

The `shadowval` frame already carries `stateGeneration`, published as 0 and never moved. Layer 3
moves it:

- **On the Link, once a second** (a sub-tick of the existing 30 Hz timer, on the message thread):
  `getStateInformation` on the tapped slot, hash the bytes, and bump `stateGeneration` when the hash
  changes.
- **On the V2:** the generation moving is a signal that *something* changed. If a parameter write
  explains it (layer 2 wrote in the last tick, or a published value moved), nothing happens. If
  nothing explains it, the V2 re-pulls the whole state by **layer 1's existing road** (`shadowState`
  on the control channel) and applies it to the copy.
- **Rate-limited to once every 2 s, never while a gesture of his is in flight, never on the audio
  thread.**

No new wire, no new file, no new layout. One field that is already published starts moving, and one
road that already works gets a second caller.

## 3. What it costs on the audio thread

**Nothing. Not "little" — nothing.** Every part of layer 3 is on a message thread in one process or
the other:

| part | thread | cost |
|---|---|---|
| the digest (`getStateInformation` + hash) | the Link's message thread, 1 Hz | measured: **26,398 bytes in 0.7–0.9 ms** on the CL 1B (layer 1 logs this on every pull) |
| publishing the generation | the Link's message thread, on the existing tick | one `uint32` in a frame already being written |
| noticing it moved | the V2's message thread, 20 Hz | one comparison |
| the re-pull and re-apply | both message threads, at most once per 2 s | one file write + read, then `setStateInformation` on the copy |

The audio thread's instruction count does not change by one.

**The real cost is the Link's message thread**, and it is the figure to watch: a plugin whose state
is megabytes, or whose `getStateInformation` is slow, would stall the tick that publishes meters.
0.9 ms per second is 0.09% of that thread. A 5 MB plugin at 50 ms per second would be 5%, which is
visible, and that is why the budget below exists rather than a hope.

## 4. How it fails safe

Five ways it can go wrong, and what each one does:

1. **The digest is dear.** Timed on every call. Over a stated budget (proposal: **5 ms**), the Link
   drops to on-demand — it digests only when the V2 asks — logs that it has, and the card says the
   view may be behind on changes made on the Link's own window. A slow plugin costs honesty, not a
   stall.
2. **The state is over the cap.** Already handled by layer 1's road: the Link refuses over 4 MB with
   its reason, and the V2 says so on the card. Layer 3 inherits that for free.
3. **The plugin refuses to hand its state over, or throws.** Layer 1's refusal, again inherited: the
   view keeps the picture it has and says the settings could not be refreshed.
4. **The re-apply throws, or the copy rejects it.** The view keeps what it has; the card says so. A
   stale picture that admits it is stale is the designed failure here, and the one thing never
   allowed is a stale picture that looks live.
5. **`setStateInformation` on a copy whose Cocoa view is already attached is the one genuinely new
   risk in the whole FOLLOW design.** Layer 1 avoids the question by seeding before the editor
   exists; layer 3 cannot. So it is behind its own switch (`kShadowFollowState`) and, if a plugin
   dies inside that call, the **repeat-crash marker already shipped** refuses the live view of *that
   plugin* by name on the next launch. This is the layer whose first bench leg must be a Softube
   plugin with its view open, with Sean at the Mac.

## 5. Cost

| piece | time |
|---|---|
| the Link's 1 Hz digest, the generation, the budget and the bail-out | 0.5 day |
| the V2's "nothing explains this" test and the rate-limited re-pull | 0.5 day |
| guards: the generation moves on a state change and not on a parameter change; the budget bails out; a refused re-apply keeps the picture and says so | 0.5 day |
| the bench leg: a state re-apply on a copy with its view open, supervised | 0.5 day |
| **total** | **~2 days** |

## 6. What I would do first

Nothing, until a plugin he actually uses is shown to need it. The honest order is **slot meters
first** (approved, wanted, and cheap), because layer 3 insures against a fault nobody has reported
while the meters answer a question he asks every session.
