# DERIVED VALUES SINCE THE MERGE BASE - the merge's safety net

Base: d68da09 (27 Aug 2026, the last commit shared with the remote before
Kathy's work). Tip: 56d7b0a (6 Sep 2026, backup/seand-mac-integration-2026-09-06)
plus the two record commits after it. After EVERY merge stage, every value
below is read back from the branch that built the binary, with its commit,
and compared with this table. ANY DIFFERENCE IS A FINDING. A merge that
silently moves one of these is the failure class this project has hit twice.

## Changed between base and tip - each with the commit and the measurement behind it
| item | where (tip) | base | now | commit | the measurement |
|---|---|---|---|---|---|
| kDefRetuneMs | EedPitchCorrect.h | 120 | 6 | 480b259 (floor), 5248699 (44), then round 40 -> 6 | round 40: retune 6 at full depth measured best (activity 4.49c, off-grid 2.33c vs 6.09 / 4.26 at 44) and Sean's ear chose it ("B is better on both"); PROVISIONAL, raw-material re-derivation owed |
| kRetuneFloorMs | EedPitchCorrect.h | absent | 6 | 480b259 | the 6 ms floor chosen by measurement (30 Aug): below it behaves as 6 |
| kMaxRetuneMs | EedPitchCorrect.h | absent (400) | 150 | e7e2b63 | measured useful correction ends near 150 ms; frames-within-3-cents cliffs past it; clamp on load with the readout memory |
| kNoteConfirmMs | EedPitchCorrect.h | 25 | 15 | b702412 (round 30) | re-derived with co-timing on: the shortest window with a clean OLD-take falsifier; round 56 notes it was set against the pre-seam-ramp mechanism and is re-derived again after the note-decision fix |
| correction_mode default | EedPitchProcessor.cpp schema | natural | custom | 5248699 (round 36), reaffirmed round 40 | the natural preset (120/55/60/nat 100) detuned the reference take; custom at the measured defaults is what ships; PROVISIONAL |
| flex, humanize defaults | schema | 55, 60 | 0, 0 | 5248699 | flex >= 25 tunes worse than dry on the reference take (round 27); humanize 0 with it |
| natural_vibrato default | schema (kNaturalVib) | 100 | 0 | 3330a0f (round 50) | at 100 the defaults render WORSE than the source on the all-voiced ruler (8.71c / 30.1% vs 6.72c / 39.8%); every ear-confirmed clip of rounds 31-46 was at 0; PROVISIONAL. The corrector's member literal natVib_ still reads 100 (dead at runtime; B8 of round 59 aligns it - held) |
| `retune` (the 0-400 dial) | new param, schema + EedRetuneMap.h | absent | present, default 0 | dee5bcb (round 46), 9f97d21 (round 51) | the round-40 v3 curve: 0 -> (6 ms, 1.0); 50 -> (80, 0.35); 100 -> (150, 0.25); 200 -> (150, 0.15); 400 -> (150, 0.10); below 50 retune 6 + 74 t^2, depth 1 - 0.65 t^0.7; median activity strictly decreasing at 18 positions; round 51: a saved state off the curve SNAPS to the nearest dial on load (nearestDial), no off-curve state |
| seam_attack_ms | new param, schema; kSeamAttackMs | absent (engine 0) | 60 | 364e587 (built at 0), the default flip to 60 after the ear gate, 0a6b013 (constructor consults the schema) | the word-start pitch-continuity ramp: the 9c seam step measured as the word-start defect; 60 ms passed the corrected bar and Sean's ear; every mode writes it |
| depth | new param, schema (kDepth) | absent | 100 | 6fcbb0a (round 31) | the slow end: retune 150 at depth 25 = 1.84c activity at 58% improve (Antares max 0.57c / 57%); ear-confirmed; since round 46 driven by the dial, ADVANCED override only, never a loaded state |
| retune_speed_ms role | schema description | the front control | INTERNAL, driven by `retune` | dee5bcb | the dial drives it; a direct write is a live override; snaps on load (round 51) |
| modes as dial positions | applyMode (EedPitchProcessor.cpp) | modes wrote retune ms + depth 100 | each mode = the dial position of its retune ms, taking the curve's retune AND depth (natural 78.6 = 120 ms / depth 29; hard = dial 0 = 6 ms) | 9f97d21 (round 51) | the dial always means what it says; a session saved in a mode reloads as that mode. THE kPresets TABLE VALUES THEMSELVES ARE UNCHANGED (see below) |
| the transport reset | EedPitchProcessor / ChainHost / PluginProcessor / LinkProcessor reset() | no override | flag-based reset at every layer | c75b222 (round 48) | positive control 132.6% RMS stale start after a locate; the fix bit-identical to a fresh instance |
| the borrow budget commit points | PluginProcessor | flipped by the registry pass at any time | committed only at prepareToPlay or a STOPPED block; pending value inert; sidecar carries publisher pid + host identity; registry pass on the processor's 1 Hz timer | 2f02c65 (round 53) | Sean's latency log: the one runtime latency change was the budget flipping mid-playback (1800 -> 18184); impulse test reported == actual in every state; L1-L8 met in host |
| key_source on load | EedPitchProcessor.cpp key setters | a load took manual | a load keeps AUTO (only a live write takes manual) | 3330a0f | every session saved in AUTO loaded as MANUAL; suite check both ways |
| kNoteChangeCents / noteChangeC_ | EedPitchCorrect.h | 90 | 90 SHIPPED; parameterised (debugNoteChangeCents) with 70 the round-59 candidate BEHIND THE FLAG, off by default | 15e51fd | round 59: 70 is the first threshold that clears the E3/F3 family at R1 (2 / 75 ms); PROVISIONAL - margin 13.5c over the measured vibrato peaks, heavy-vibrato bounce owed |
| noteDecExp_ (C1/C2 flag) | EedPitchCorrect.h | absent | present, DEFAULT 0 = bit-identical | 15e51fd | the wrong-note-at-transitions fix behind the investigation flag; NOT shipped; R4 stop pending the C1 split ruling |
| kUidGateFloorMs | LinkShm.h (UidClaimGate) | absent (5 observations, no time) | 3500 ms + 5 observations, OR a dead publisher pid -> adopt at once | 6 Sep 2026 (C4a/C4b, shoot day) | DERIVED from the product's own freshness window: a row whose heartbeat has not moved for 3500 ms is already not fresh to the Link's solo scan (LinkProcessor.cpp `fresh = (nowMs - lastHbMoveMs) < 3500.0`) and stale to the main plugin (~3 s); measured: the count-only gate adopted a LIVE sibling 2 of 4 runs; with the floor L4 20/20 and burst 20/20; the Pro Tools storm (incarnations 0.7-1.2 s, 30 in 30 s) burns 0 identities, leaves 0 orphans, and the first survivor registers 4389 ms after it; a dead-pid predecessor is adopted in 25 ms |

## Unchanged between base and tip - and why they are watched
| item | value | why it matters |
|---|---|---|
| kBorrowAlignBudgetFrames | 16384 | the rack-borrow alignment budget (1024 cushion + 15360 headroom); its 341 ms is what the host compensates when a capable Link is present; on the eighteen-constant register for re-derivation, NOT in scope of any round so far |
| kGapIsNoteChangeMs | 200 | the same-note rule across a gap; round 59's C2 works WITHIN it (a shorter gap re-decides the target for one confirm window without touching this) |
| kSeamFadeMs | 1.5 | the seam cross-fade; measured 1.5/10/25 - short wins (DESIGN_SEAM_RESIDUAL.md) |
| targeting_ignores_vibrato default | on (1.0) in the schema, and true in all four presets | ON is a necessary half of the wrong-note condition (round 58: legacy path AND IGN VIB on); turning it off is NOT the fix and is not shipped silently |
| kPresets values | natural 120/55/60/nat 100; balanced 40/25/30/100; tuned 8/0/0/40; hard 0/0/0/0; ignoreVib true in all | the spec's mode table; unchanged in value, re-interpreted by round 51 (the retune column selects a dial position) |
| kNoteConfirmMs usage as the C2 window | 15 | C2 reuses the confirm window for the resume re-decision; held with round 59 |
| the F0JumpGate constants | 600c big jump, 200c same-candidate, 50 ms confirm, 30 ms forget | untouched by every round; the gate fired on nothing in the traced windows |

## How to use this after a merge stage
Read each "now" value back from the merged tree (grep the constant; grep the
schema line; open applyMode for the dial mapping; run the pitch mode test -
its ledgers and the round-46/51 checks assert the dial curve, the snap on
load, the defaults and key_source on load). Print the merged commit beside
every value. Any value that differs from "now" is a finding to be traced to
a commit before it is accepted or reverted - never resolved by preference.
