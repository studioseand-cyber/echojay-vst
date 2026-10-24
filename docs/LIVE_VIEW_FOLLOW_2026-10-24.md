# The live view, both directions — FOLLOW then CONTROL

**Sean's ask, 24 Oct 2026, after the 14:27 crash was proven fixed on D4B29BB7:**

> *"1. FOLLOW (first): the V2's copy mirrors the real plugin on the Link continuously: every
> parameter change and any state the plugin keeps outside its parameters. A change made on the
> Link's own window shows in the V2's view within a moment, and the copy's meters then match the
> real plugin. Today the copy keeps its starting settings, so its view and meters drift from the
> real one. 2. CONTROL (straight after) ... and then the "View only" lock comes off. Send me a
> short design note first (how changes travel both ways, latency, plugins whose state isn't all
> parameters, what happens while an agent run is dialling that plugin, and how a crash in the copy
> is contained)."*

This note supersedes §2 of `LIVE_VIEW_CONTROL_2026-10-24.md` (which put FOLLOW second and designed
it as a new feedback block on the meter frame — the wrong carrier, see F1 below). §1, §5 and §6 of
that note still stand for CONTROL.

---

## 0. Today, exactly

The copy is created from the plugin description and **nothing is ever applied to it**
(`shadowFollowSelection` → `asyncCreatePlugin` → `shadowBegin`; no state, no parameters). So it
opens at the plugin's factory defaults, or — since 1b0732be — at whatever the last parked copy of
that plugin was left holding. His sentence *"the copy keeps its starting settings"* is literally
what the code does.

The two things that make the picture wrong are therefore **one missing initial sync** and **one
missing continuous mirror**, in that order of value. F1 below is half a day and fixes most of what
he saw.

## 1. FOLLOW: three layers, cheapest first

**F1 — the baseline, on tap. The Link side is already built.** The control channel has served
`pullSlotState` since the edit session: the Link calls `getStateInformation` on that slot's real
plugin, refuses over 4 MB (`kLinkTransferMaxSlotBytes`) or if the plugin throws, and returns it
base64 in the ctrl-ack. The live view asks for it in the same request that asks for the tap, and
applies it with `setStateInformation` to the copy **before** the first block and **before** the
editor is created. That is the whole of "any state the plugin keeps outside its parameters", for
free, on a road that has been carrying real plugin chunks for weeks.

**F2 — the continuous mirror, 30 Hz, parameters.** The Link's timer already runs at 30 Hz and
already publishes a meter frame on every tick. It publishes, on the same tick and only while a slot
is tapped, a small seqlock'd snapshot of **that one slot's** parameter values:

```
  shadowval_<uid>.bin   (new, ~4 KB, fixed, mmap'd, seqlock'd — the LinkMeterFrame shape)
    seq, structureRevision, slot, paramCount, schemaDigest, stateGeneration
    float value[kShadowValMax]        // normalised 0..1, the parameter's own index
```

A **new file, not a new field**: `LinkMeterFrame` is 128 bytes and its size *is* the stride of the
array, and the audio ring's header is frozen field by field. A separate small file costs one mmap,
needs no layout change on any shipped structure, and its **absence is the natural "an older Link"
answer** — which is this house's downgrade rule.

Written by the Link's **message** thread, read by the V2's **message** thread (its editor timer is
20 Hz). Nothing touches the audio thread, in either process.

The V2 writes only the values that **changed** since its last apply, through
`setValueNotifyingHost` on the copy's own parameter, so steady state is zero plugin calls and a
knob move is one. `schemaDigest` (parameter count plus a hash of the parameter names) is checked
first: if it disagrees, follow is **refused with a line** rather than applied by index — an index
without an identity behind it is the proxy-for-identity family, and we have paid for that one.

**F3 — non-parameter drift.** Some changes on the Link's own window report no parameter: a graph
drag, an IR load, a preset recall. The Link therefore digests the tapped slot's state blob on a
1 Hz sub-tick and publishes `stateGeneration`. When the generation moves and no parameter change
explains it, the V2 re-pulls (F1's road) and re-applies — **rate-limited to once every 2 s, never
while a gesture of his is in flight, never on the audio thread.**

## 2. CONTROL: the ten lines that were never written

Unchanged from the previous note, and it is smaller than FOLLOW: the value ring, the gesture
framing (`kBegin`/`kStream`/`kEnd`, one undo step per drag) and the stale-structure discard are all
built and proven. The Link *drains* `kSetParam` today and throws it away by ruling, counting it into
`ringSetParamPending_`. C1 is the apply on the Link's message tick, coalesced per `(slot, paramId)`;
C2 is a parameter listener on the copy that writes the ring; then `kLiveViewControlBuilt` goes true
and the shield comes down.

**The copy is never the authority.** The Link's value is the fact; when it lands, the copy is
reconciled to what landed. Which is exactly F2, so CONTROL gets its acknowledgement for free once
FOLLOW exists — and that is the real reason FOLLOW goes first.

## 3. Latency, with the clock stated

| direction | hop | cost |
|---|---|---|
| FOLLOW, parameter | Link's 30 Hz tick | ≤ 33 ms |
| | V2's 20 Hz editor tick | ≤ 50 ms |
| | **knob on the Link → seen in the view** | **35–85 ms** |
| FOLLOW, non-parameter | 1 Hz digest + the pull round-trip | ~1–1.5 s |
| FOLLOW, baseline (F1) | one pull before the view opens | it is the view's own open time |
| CONTROL | copy → ring → Link's drain → apply tick → heard | 25–50 ms (one block = 10.7 ms at 512/48k) |

85 ms is inside "within a moment" for a picture. It is **not** inside it for audio, which is why
CONTROL does not travel this way round — it uses the ring.

## 4. Plugins whose state is not all parameters

The two directions are **not symmetric**, and that asymmetry decides §4 of the previous note:

- **FOLLOW can carry a whole state blob**, because the thing receiving it is a disposable copy
  outside the signal path. A long `setStateInformation` on it costs a late picture, nothing else.
- **CONTROL cannot.** The thing receiving it would be the plugin *in the signal path*:
  `setStateInformation` is a long call on the Link's message thread, it can re-instantiate the
  plugin's own view, and it cannot be done per gesture. So a change the copy makes outside its
  parameters **does not travel**, and the view says so on the card — *"this change can't be sent —
  open <plugin> on the Link to make it there"* — with the button that does exactly that. The edit
  session stays the road for a whole-state change, which is what the edit session is.

So: everything follows; **reported parameters** control. No silent loss either way, which was his
standard this morning.

## 5. An agent run dialling that plugin

- **FOLLOW needs no arbitration at all.** It is read-only and the Link is the authority, so while a
  run dials that slot the view simply follows the run — his picture moves as the agent works. That
  is a bonus of the direction, not extra work.
- **CONTROL does.** `kSetParam` is documented `HUMAN GESTURE only`; a run's road is the chain-cmd v2
  `editOps` file channel, and that separation is law. While a run holds that rack a gesture on the
  view is **refused with the reason** (*"an agent is working on this rack — stop it, or wait"*);
  while a human gesture is in flight (`kBegin`…`kEnd`) the run's next op on that slot waits for the
  `kEnd`, because a drag is one step and must not be split.

## 6. How a crash in the copy is contained — honestly

**It is not contained by the process boundary, and it cannot be.** The V2 and the Link are two AU
instances in the *same* `AUHostingServiceXPC` process, so a crash in the copy takes Logic's audio
units down with it — which is precisely what the three live-view crashes did. Real containment means
hosting the copy in a third process with its own view, which is weeks of work and is also what the
Link already is (hence "open it on the Link" as the permanent fallback).

What FOLLOW does about it, in order of value:

1. **Fewer plugin calls, not more.** Changed-only parameter writes mean steady state costs the
   plugin nothing. A blob re-apply happens on a generation change, at most once every 2 s, never
   during a gesture.
2. **A switch per layer** (`kShadowFollowState`, `kShadowFollowParams`), next to
   `kShadowViewEnabled`, so a layer that hurts a plugin goes off in the build it is found in — the
   discipline that already got the view back in front of him.
3. **A crash cookie, new and cheap (~0.5 day).** The V2 writes a marker naming the plugin and the
   layer *before* the first risky call and clears it on a clean teardown. On the next launch, a
   marker that is still there means that plugin crashed us last time: the live view is refused for
   **that plugin** with a line saying so and an offer to try again. This does not stop the first
   crash. It stops the second, third and fourth, which is the part that costs him his session.
4. The existing rules still carry most of the weight: never dispose a hosted AU mid-session, editor
   before processor, every declared bus fed, the plugin's own layout, withheld plugins never
   instantiated.

## 7. Cost

| stage | what | time |
|---|---|---|
| F1 | pull the real state on tap, apply before the first block and before the editor | **0.5 day** |
| F2 | the `shadowval` snapshot, Link writer + V2 reader, changed-only, schema digest | **1 day** |
| F3 | the 1 Hz state digest, generation, rate-limited re-pull | **0.5 day** |
| F4 | the crash cookie and the per-layer switches | **0.5 day** |
| G | guards: F1 applied-before-first-block; a digest mismatch refuses; changed-only writes nothing when nothing moved; the follow does not fight a gesture; the cookie refuses on known-bad and passes on known-good | **1 day** |
| **FOLLOW total** | | **~3.5 days** |
| C1–C3, D2 | CONTROL, per §1/§5 of the previous note | ~2.5 days |

## 8. What I have not established

- **The cost of a full parameter sweep per 30 Hz tick on the Link's message thread** for a
  200-parameter plugin. F2 must measure it and the guard must assert the tick's budget in
  wall-clock, not in counts. If it is too dear, the sweep goes round-robin in chunks and the follow
  latency rises — which is a figure to state, not a thing to hide.
- **The cost and size of `getStateInformation` on the tapped slot at 1 Hz.** The pull road caps at
  4 MB but has never been timed per second. F3 bails out to on-demand if it is dear, and says so.
- **Whether a guest AU's own view redraws when the host writes its parameter.** It should —
  `setValueNotifyingHost` reaches an AU's parameter-listener road — but it is a claim until the
  Tube-Tech is driven on the bench, under the supervised exception. That is the first bench leg.
- **Whether `setStateInformation` on a copy whose Cocoa view is attached is safe** on the Softube
  plugins. F1 applies the state *before* the editor exists, which avoids the question entirely;
  F3 cannot, and that is the one genuinely new risk in FOLLOW.
