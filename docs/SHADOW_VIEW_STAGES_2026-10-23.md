# The shadow view: what this round covers, and what control still needs (23 October 2026)

Written before any of it is built, because Sean asked for the note first: *"Before you build, write a
short note in docs saying which stages this covers and what is left for control later. If the design
has a choice I haven't ruled on, take the read-only option and list it in the note."*

The design is `docs/PLAN_LIVE_SHADOW_2026-10-20.md`. Its stages are A (the slot-input tap and the
ring), B (the V2's hosted copy, fed from that ring, output discarded), C (the parameter follow, both
ways) and D (the refusals and their guards).

**His ruling for this round:** *"as a VIEW ONLY: when I select a Link's plugin in the V2, I see that
plugin's real editor live in the V2 (moving meters and gain reduction) without opening the Link's
window... No control through it yet: clicks do nothing, and it must never take the rack, stop a run or
change ownership."*

---

## 1. What this round covers

| Stage | This round | Note |
|---|---|---|
| **A** — the slot-input tap | **the tap object already shipped** (`Source/EJShadowTap.h`, in the graph on every slot's pre-trim node since 21 Oct) | What it never had is a RING to push into and anyone to tell it which slot. Both are this round. |
| **A (write side)** | `shadow_<uid>.bin`, created by the Link, written only while a slot is asked for | Its own ring, not a second consumer of `audio_<uid>.bin`: `readIdx` is one cursor, so sharing it would let the shadow steal frames from a capture (the plan, §1). |
| **A (selector)** | one new command on the existing V2→Link ring: "shadow slot N", and −1 for "stop" | Append-only in `EJCmdRing::Kind`, because the two binaries are built separately and a renumbering is a silent mis-application. |
| **B** — the V2's copy | `Source/EJShadowView.h` wired up: map the ring, host one prepared copy, pump it from `processBlock`, throw the output away | The file was written on 21 Oct and was wired to nothing. |
| **B (the view)** | the copy's own editor shown in the V2's panel, in place of *"The live view of this plugin is coming soon"* | This is the thing he asked to see. |
| **D (part)** | the refusals the view cannot ship without: a second instance that fails or prompts, a sample-rate mismatch, leaving the slot, and no ring | Per the plan's §4: created on the message thread with a time bound, never at a transport edge, and a failure falls back to the static picture with one plain line. |

## 2. What is NOT in this round, and what control will need

| Stage | Left for later | Why it is not needed for a view |
|---|---|---|
| **C** — the follow, V2 → Link | a knob turned on the copy becomes a `kSetParam` gesture on the Link | Clicks do nothing this round, by his ruling, so nothing travels upstream. |
| **C** — the follow, Link → V2 | the copy follows the Link's values continuously, by schema id, structure-revision checked, with the echo absorbed | The copy is instantiated with the slot's **published settings** as its starting point and then left alone. It therefore shows the real plugin's METERS live and its KNOBS as of the moment it was opened. §4 below is explicit about that limit. |
| **D** — the rest | the CPU bound, the ring exclusion against a live capture, and the per-plugin record of what refused | The first two are real and bounded (one slot at a time, zero cost when off); the record is an optimisation of the refusal, not the refusal. |

## 3. The choices he has not ruled on, taken the read-only way

1. **A knob on the copy does nothing at all**, rather than being hidden or disabled-looking. His
   ruling says "clicks do nothing"; a guest plugin's own editor cannot be asked to grey its controls,
   so the copy's editor is shown as it is and its gestures are dropped on this side. The card says so
   in one line, so the view never pretends to be a control surface.
2. **The copy's knobs are a snapshot, its meters are live.** Without stage C the copy does not follow
   a value the Link changes after the view opened. Read-only option taken: the view is labelled as the
   plugin's own live picture, and the **static settings list stays** beside it as the authority on
   values (it is read from the Link's sidecar, which is the parameter authority).
3. **One slot at a time, and only while it is in view.** Leaving the slot stops the tap. Not a
   preference: the tap takes one slot and the ring is one per Link.
4. **No borrow, no lease, no ownership change of any kind.** The shadow asks the Link for audio and
   nothing else. This is also now a guarded claim rather than an intention, because his 08:44 fault was
   exactly an ownership change nobody asked for.
5. **A licence-checked plugin that refuses or prompts is never retried in the same session**, and the
   view falls back to the static picture naming the plugin. The alternative (retry on each selection)
   risks an iLok dialog on his Mac, which is the one thing that must never happen.

## 4. What the view will and will not tell him

- **Will:** the plugin's own meters, its gain reduction, its needles and its animation, moving against
  the signal the real plugin hears (the tap is at the slot's input, after its pre-trim — "the signal
  the plugin actually hears").
- **Will not:** be sample-accurate with the real one. The cushion is 1024 frames with an 8192-frame
  re-seek trip, so the picture lags by about 25 ms at 48 kHz / 512, plus up to one block if the host
  runs the V2 before the Link. For level and GR movement that is invisible; for a transient-accurate
  needle it is not zero, and no line in the product claims the two are the same instant.
- **Will not:** change what is heard. The copy's output is filled into a scratch buffer and discarded;
  the Link remains the only real processor.

## 5. Where it is proven, and where it is not

- Guards: the ring's name and layout, the selector command's wire number, the refusal decisions, and
  "the shadow changes no ownership" are single-guard claims.
- The rig: a row selects a Link's plugin in the V2 and asserts the view comes up, the run is
  untouched and ownership is untouched. **The rig's row uses an EchoJay plugin only** — the standing
  rule is that the rig never loads a PACE or iLok plugin, so the licence refusal path is NOT exercised
  there and is named as owed.
- **Pro Tools is not proven this round.** `tools/quick.sh` builds four targets and no AAX, and the
  pair are AAX in the Pro Tools engine; a Pro Tools proof needs a full-gate build and his own test.
