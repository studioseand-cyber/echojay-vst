# Morning report, 17 October 2026

## Installed

**(filled in when the chain finishes - see "The night's chain" below.)**

---

## What is fixed, each with the row that was RED on 16b and GREEN on this build

Your method, followed exactly: every failure became a matrix row, the rows were run against the
INSTALLED ship_2026-10-16b first to prove they reproduce what you saw, then against this build.

**The red capture on 16b: 10 rows RED** - the eight new ones below plus A10 and B5, which both failed
on the same raw prefix and are independent proof that your step 8 was real.

| your item | the row | on 16b | on this build |
|---|---|---|---|
| 1. Suggested Settings stale (3 of 3) | **I9b** own rack, **I9c** the Link's mirrored card | RED: "the plugin reads Threshold=-30 and the card reads 'threshold 0'" | GREEN |
| 2. conversation continuity (my half) | **E9** the start body | RED: no `recentChat`, no `transport` | GREEN |
| 3. new instruction while waiting on Apply | **B6** | RED (the refusal line) | GREEN |
| 4. a tap still needed Apply | **B2b** + guard A9a/A9b | RED: still asking for a press | GREEN |
| 5. duplicate closing | **G7b** | RED: the sentence drawn twice | GREEN |
| 6. the mirror's "Failed" | **T5** | RED: "Failed: ... editor could not be created" | GREEN |
| 7. can't-load offered anyway | **P1** | RED: the unloadable AU was in the inventory | GREEN |
| 8. raw "op 1 invalid:" | **I4c**, and A10/B5 | RED on all three | GREEN |

### What each fix actually was

1. **The card.** One line in `recordApplyReport`: the card was rewritten only `if
   (!appliedSummary.isEmpty())`, and the settle pass that finally read the real value had nothing in
   that list - the value was already there. The card is now composed from `dialLandedValues`, the
   ledger the step line quotes, in the function that writes it. My first attempt put the hook in the
   CALLER and the matrix caught it: the settle road calls `recordApplyReport` directly.
2. **Continuity, my half.** The start body carries `recentChat` (six turns, your messages and the
   agent's) and `context.transport` with known/playing/heardSeconds/signalNow. B's half - a run that
   needs you to act ending as an ask, your reply resuming THAT job, and no offer-ask after a
   read-only answer - is FOR_B §16 with rows E10-E13.
3. **#21 covers any waiting run.** It was wired to the ask road only. A run waiting on YOU
   (awaiting_approval, awaiting_ask, listening) is a question in another shape, so a new instruction
   stops it and starts its own; the refusal stays for a run that is genuinely mid-flight.
4. **The tap is the approval.** The tapped label is remembered and spent on the step that carries out
   that pick, recording itself as the cross-channel confirmation at the same moment. Only that step:
   anything else ask_first still waits, which the guard holds in both directions.
5. **One sentence, drawn once.** On a short run the streamed sentence IS the done summary, so it was
   drawn as the message's content and again as the row's closing. The row drops the repeat.
6. **The mirror says where the plugin is** - "Renaissance Axx is running on 88_7even Kick_2_01, open
   it in that track's EchoJay window" - with a "Bring it forward" button that asks that Link over its
   control channel and then says which of the two happened. Nothing here can make Logic OPEN a shut
   window; NEEDS_SEAN D19 is explicit about that.
7. **Nothing unloadable reaches the inventory.** The architecture gate judged VST3 rows only - "AU
   rows are never judged", in a comment, because an AU's identifier is not a path. AU bundles are now
   found by name in the two Components directories and judged by their slices.
8. **The validator's prefix is translated** at the one seam where a rack's refusal becomes a line on
   the row, and I4's deny-list bans the phrase. My first attempt tested the whole string; the real
   refusal is two lines and the prefix was on the second.

---

## What is queued, and why

- **Item 9, Link meters to the main plugin** - the FIRST item of the next round
  (`NEXT_ROUND_QUEUE` item 0), with its design written out: a per-slot meter frame published by the
  Link, a reader that can tell "not published" from "zero reduction", the certification rule riding
  with the frame, and the listen window with it. NEEDS_SEAN **D18** says why it was not started
  tonight: getting it wrong produces a figure that is WRONG rather than absent, and I would have been
  writing it blind against a plugin I cannot open in Logic until you are awake.
- **A redo-aware undo history** (your D17 ruling) - after launch, item 10.
- **The AU sweep's locking** - item 11, a one-off disk read under `archMutex_`, noted rather than
  fixed mid-build.
- **The chain ack naming the slot an add created** - item 9, which would turn the honest
  "I cannot tell which of these copies I just added" into an exact answer.

---

## What the runner could NOT test, and why you are the instrument for it

**The Link half of the live pass.** The live rows that drive a REAL model and then read the Link's
sidecar ran 3 of 5 on `A4-live`, and the two failures are the measuring equipment, not the build.
When the rig registers a Link component and asks the host to instantiate it, macOS resolves the
AudioUnit by its IDENTIFIER, and the identifier belongs to the component INSTALLED in
`~/Library/Audio/Plug-Ins/Components` - last night's `ship_2026-10-16b`. So the V2 under test was
tonight's binary and the Link answering it was yesterday's, which has neither the dial-pending wait
nor the card composition that row asserts. I proved it by size: the two binaries differ, and the
runner now prints both figures and SKIPS those rows rather than passing or failing them, because a
new V2 talking to an old Link is a configuration that never ships and a verdict from it would be
about nothing. `A3-live` - the same add on the plugin's OWN rack, where no second component is
involved - held 5 of 5 against the real model.

That is `NEXT_ROUND_QUEUE` **item 0b**: host the Link BY PATH in the rig, so the runner tests what
it was handed. Until then step 1 of the recheck below is the only proof of the Link side, and it is
your eyes on it.

---

## A short Logic recheck, only tonight's failures (about 6 minutes)

Agent mode **Ask**, the project with the Mix Bus and the Kick's Link, something playing.

1. **The card** (your #1). Mix Bus: "add Renaissance Axx with threshold at -20 dB". When it finishes,
   select that slot. **PASS:** Suggested Settings reads **-20**, not 0. Then the same on the Kick's
   Link, and check its slot in the main window's mirror: **also -20**.
2. **The tap** (#4). Kick's Link: "add a compressor". When the chips appear, **tap one**.
   **PASS:** it is added with no Apply press - and the line still names the track.
3. **A new instruction while it waits** (#3). Ask for something that needs an Apply ("build a chain
   on the kick"), and while it waits type **"set the pre-gain to 2 dB"**. **PASS:** the waiting run
   is marked stopped and a NEW run sets the pre-gain. **FAIL:** "The agent is still working...".
4. **The closing** (#5). Any short run. **PASS:** its sentence appears **once** - not as a bubble
   above the row and again inside it.
5. **The mirror** (#6). Select the Renaissance Axx slot in the main window's mirror of the Link.
   **PASS:** "Renaissance Axx is running on 88_7even Kick_2_01, open it in that track's EchoJay
   window" and a **Bring it forward** button - no red "Failed". Press the button: either that window
   comes forward, or it says Logic owns it. Both are honest answers.
6. **Pro-C 2** (#7). "Add Pro-C 2 to this rack". **PASS:** it is never put behind an Apply - either
   the nearest installed match is offered, or it says plainly that it cannot load here.
7. **A refusal in words** (#8). Ask for something impossible on the Kick's Link (e.g. "add
   kHs Transient Shaper"). **PASS:** the failed line reads as a sentence - no "op 1 invalid:".
8. **Continuity** (#2, with B). With audio playing: "how much gain reduction is the compressor
   doing?" **PASS:** it listens and answers with a figure. If it asks you to play, reply "im playing
   it": **PASS:** it resumes the SAME question (gain reduction), not loudness. This half depends on
   B's change as well as mine; if it still drifts, that is FOR_B §16 and the evidence is in it.
