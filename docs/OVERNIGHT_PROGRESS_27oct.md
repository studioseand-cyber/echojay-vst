# Overnight, 26-27 Oct 2026: make the real editor work, or say why not
### His ruling of 21:08. Re-homing approved as a change to §2. Install only on an all-green gate.

Rules for the night: HEAVY.lock for every build/rig/gate, **B has priority** for its production deploy
and its gates; skip anything blocked and come back to it; the switch stays off and hidden; the copy
road is not touched.

- [x] 1. The switch is hidden from Settings in an installed build (dev-flagged), still flippable by a row
- [x] 2. Re-homing: `lenderRehomeSlotView` - the lender moves its own view back, one addSubview, never remove-then-add
- [x] 3. The editor is KEPT across a hand-back (one per instance), destroyed only by the teardowns
- [x] 4. One press = one take: the pane is idempotent, and a row counts the lender's creates
- [x] 5. Guard legs for the new rules (EL/RH), red on known-bad
- [x] 6. Third-party subjects chosen, screened and seeded by identifier:
         **MCompressor** (MeldaProduction - MTransient is not installed on this Mac; same maker and UI
         framework), **ValhallaDelay** (Valhalla DSP, large custom interface), **TDR SlickEQ M**
         (Tokyo Dawn Labs, JUCE-built), **Fresh Air** (Slate Digital). None links PACE/iLok, none has
         an auth framework, and EJPaceCheck::isPaceWrapped refuses them if that ever changes.
- [x] 7. RH rows: 50x open/close/reopen per plugin; slot switch; slot replace; delete while shown;
         hand-back both ways repeatedly; track deleted; session closed while shown; V2 window closed
- [ ] 8. The whole gate green with none skipped (LV + RE + RH + the guards)
- [ ] 9. Install, verify UUIDs in the installed binaries, switch still off
- [x] 10. docs/MORNING_REPORT_2026-10-27.md + ~/Desktop/FOR_REVIEW_22oct/A_NIGHT_DONE_27

If re-homing cannot be made crash-free on those plugins: stop, write up why, leave the copy road as the
live view. No more patching.


## Log

- 21:10 switch hidden outside dev mode (the tick only; the file, the predicate and the rig's press are
  unchanged), and both readers of the toggle list now skip a hidden one so no gap is left behind.
- 21:12 re-homing built: `lenderRehomeSlotView` + `NativeClip2_parkGuestView` (one addSubview into a
  windowless parking view this bundle owns; nothing is removed first). A returned lease now re-homes
  and KEEPS the editor; teardowns still destroy it.
- 21:14 the lease counts editors built per slot, so "one press = one take" is measured, not asserted.
- 21:20 RH1-RH8 written: the stress list per plugin, with per-subject mock plans and the subject's OWN
  slot discovered rather than assumed.

- 23:11 **RE0-RE15 all green**: the real editor shows with no copy, sits still in pixels, a knob stays on
  the Link's plugin across a close and reopen, the hand-back runs three laps each way, the slot replace
  lets go, and the switch puts the view back on the copy road (that last one needed a product change:
  turning the switch off now returns a lent editor at once).
- 00:18 **RH1/RH2 green**: all four subjects load on the Link, and fifty opens and closes per plugin
  build exactly ONE editor each - his one-press-one-take and the leak test, on MCompressor,
  ValhallaDelay, TDR SlickEQ M and AUDelay, with every panel steady.
- 00:30 ValhallaDelay crashes in its OWN thread when its host goes away (EXC_BAD_ACCESS at 0x0,
  pthread_mutex_lock, no EchoJay frame) - and identically with the switch OFF, measured. Not the real
  editor's; recorded as its own finding. RH6 uses Apple's AUDelay instead.
- 00:40 the suite now prints its verdict BEFORE tearing the tracks down: four passes had reported
  "47 rows, 0 verdicts" because that plugin's shutdown crash ate the result after every row had been
  decided.

- 01:40 RE11 green once its floor was measured in both configurations: the view's presence moves the
  V2's output by 0.000000000 against a floor of 0.0000004 (the own rack's reverb tail).
- 01:55 **RH3 is the one row not proven.** It PASSES alone; in the full pass the open takes the copy
  road with the pane saying "the real-editor switch is off" moments after the row verified it ON
  through the plugin's own road. Contradiction logged, not resolved. NOT INSTALLED, per his rule.
- [ ] 8. (the gate is 90/91 - RH3 open)   - [ ] 9. (no install, by his rule)

---

## 27 Oct, morning: what the full pass found (A)

1. **The ValhallaDelay crash is not ours and not the track delete** - it is static destruction on the way out
   of the process, reproduced in a process with no EchoJay object in it. `docs/VALHALLA_EXIT_CRASH_2026-10-27.md`.
   host_rig now leaves by `_Exit`; `tools/hosted_exit_probe` is the guard, red in both directions.
2. **MCompressor is PACE-wrapped** and was loaded in the rig overnight against the standing rule. Every
   identifier the rig enumerates is now screened by EJPaceCheck; FabFilter Pro-C 3 takes the third maker's place.
3. **RH3's "switch-state contradiction" was not the switch.** The pane was showing the V2's own rack's plugin
   behind the Link's cards: the refresh asked "is the open slot index the selected one" and a slot index does
   not name a rack. Fixed as a pure function (`echojay::inlineNeedsReopen`), guard legs IV0-IV6
   (chat_store_guard GREEN 366), new rig row RH9, and two new state fields (`inlinePlugin`, `inlineRack`) so a
   row can read WHOSE plugin is on screen rather than only which road.
4. **And the full pass found what the filtered run could not: the real editor only opens where an edit
   session is already engaged.** In the full pass the session was on `6c8b649d5d` while the live-view rows
   worked on `e80636c4fb`, and the real road produced **zero** lent editors (the pane reports the "none"
   road). Overnight every lent editor and every session renewal was the same uid (`1229413db5`), so the
   precondition was armed by accident and RE8-RE15 and RH1-RH8 were green under it.
   The cause is one line: `ChainListPanel::selectSlot` returns early for a remote rack unless
   `editingSession && i == sessionSlot`, and on a remote rack that early return is the ONLY thing between a
   selection and `showInline`. The copy road does not need a session - it opens from
   `shadowFollowSelection`'s tap-ack - so with the switch ON and no session, nothing opens at all.
   **This is a product fault, not a row fault**, and it is the next thing to fix: ask for the real editor in
   the place that already decided the road.

Nothing is installed. Sean remains on V2 AU 9B29E0FE / Link AU F95EC755.

---

## 27 Oct, evening: round 14 (3 reds of 205 rows), and one product fault among them

The full unfiltered pass, one process, five open/close laps per plugin (his ruling: the full fifty is
for the final gate and for overnight runs). **RED: 3 failed, 202 passed, 18 skipped** - against round
9's 5 failed / 195 passed / 20 skipped. Build V2 `178108C3` / Link `93F3FE99`, not installed.

### 1. RE14 - the V2 took the Link's editor back BY ITSELF, 183 ms after letting go (product)

The row's trail said "lap 3: to the Link NO ... the Link said *Showing AUMultibandCompressor's own
window, live on the Link*" - both sides claiming the plugin, which is the state the row exists to
catch. The log settles who was right:

```
18:37:22.419  the rig asks the Link's own window for slot 1
18:37:22.478  report_state seq=250      <- the rig's sample, 19 ms BEFORE the hand-over
18:37:22.497  the V2 lets go; the Link takes its editor back and builds its own (7th for that slot)
18:37:22.674  EJPanel: rebuild slots=2 sel=0
18:37:22.680  "closed this Link's own window ... because the plugin was opened in the V2's live view"
18:37:22.705  report_state seq=251      <- the next sample: the V2 holds it again
```

No `open_slot` command sits between the ask and that re-take: **nothing pressed anything**. The
refresh tick found the pane empty for the selected slot and asked for the lease again.

The revoke suppression I added this afternoon was supposed to stop exactly this, and it did not,
because `ChainListPanel::rebuild` only consults `canShowSlotEditorHere` in the branch guarded by
`remote && !editingSession` - and an edit session had been engaged on that very Link since
18:37:12.386 (`EJBorrow: session ACTIVE <- true`, never released before the laps). With a session up,
the revoke was never asked about.

Fixed where the lease is actually taken rather than at the panel's two reopen sites: `showInline` now
asks `echojay::mayTakeLentEditor (deliberate, revokeStandsHere)` before it calls `onCreateLentEditor`,
and refuses an AUTOMATIC take while a revoke stands for that rack and slot. A session's own editing
copy is not the Link's instance and cannot be in two windows, so it still opens as it always did.
Guard legs RV0-RV4 in chat_store_guard, RV0 carrying the known-bad behaviour (`return true`).

### 2. R5 and SCEN A4 - the suite was measuring a world with no Link in it (the rig)

R5 answered `view= working=` and SCEN A4 read `slots -1 -> -1`. The restoration that puts back the
tracks the live-view rows delete DID run and DID re-create both Links - and then printed "no connected
Link came back within 20 s". It waited with `rig.until`, **which pumps the message thread and nothing
else**, while a Link is only in the registry as connected for as long as it is passing audio (that is
why R0 gets its registry rows from a 200-block loop, not from a wait). Twenty seconds of silence, and
every Link-dependent leg after it inherited an empty world: R5 red, SCEN A4 red, R1b and B9 skipped.
The wait now plays the tracks while it waits, in the same shape as R0's loop.

**B9 is a live-view row**, so by his 26 Oct rule its skip is a stop on its own - it is in this fix.

### 3. What the reds printer missed

Three failures, two printed: `SCEN A4 FAIL` arrived with stdout noise ahead of it
(`eMacOS_Cocoa_Runtime eInit_RM ...`) and the `^  ROW .*FAIL` pattern did not match it. Round 15's
printer matches every shape of failing row. A verdict line that says 3 and a list that shows 2 is the
kind of thing that gets read as "two reds".

---

## 27 Oct, evening: the queue after the real editor (A0 and what K's branch needs)

Sean's go-ahead, 27 Oct evening: the `EJMatchTab` extraction and the `k-base-1` tag on 28 Oct, after the
real-editor gate, with a gate of their own. K starts today on the zero-conflict work from `cea66b4e`.

- [x] **A0.1 C2, the Match host boundary** - `docs/CONTRACT_MATCH_HOST_2026-10-27.md`, copy in
      `FOR_REVIEW_22oct`. The whole `MatchHost` interface (including the calls K asked for:
      `openReferenceBrowser`, `seekCompareStream`, `verifyCapture`, `postToSessionChat`,
      `openSessionChatAt`, `pluginVersion`), file ownership, the `match_report` payload, and §5 the hop
      accumulator.
- [x] **A0.2 C5, do the three G1 ops reach a Link** - `FOR_B_C5_GAIN_OPS_TO_A_LINK_2026-10-27.md`.
      **All three do.** `set_pre_gain` and `set_io` ride the chain-edit channel with sidecar read-backs;
      `set_rack_out` is NOT an edit op - routed before `translate` to `setRackOutDb`, which writes the
      control-channel `gainDb`, clamps +/-24 dB, confirms from the registry row within 3 s and has its own
      `linkGain` undo kind. My first draft said it did not reach a Link at all, concluding from the silence
      of the op table and the control keys; corrected, and B is holding for the corrected version.
- [ ] **A0.3 the extraction** - `EJMatchTab.*` behind `MatchHost`, no behaviour change, the Match rows of
      `mapfps_test` and `ui_guard` green as they are. 28 Oct, its own gate.
- [ ] **A0.4 cut `k-base-1`** once the extraction passes, and tell K.
- [ ] **The hop accumulator** (C2 §5), K's specification, mine to build: one 60-byte frame per
      sample-counted 100 ms LUFS block, both sides, 20-minute cap, read at `stopCapture` with no lock.
      It is a fourth consumer of `perOct`, tapped at the same line as the other three.
- [ ] **A real-time allocation already in that function**: `allBlocks.push_back` runs once per hop for the
      whole capture, unbounded, on the audio thread. Found while siting the accumulator. Not K's, not the
      contract's, mine.
- [ ] **A1** remaining-work §2 (the dial as its own step, slot dial, dial ranges, `clip_drive`), then
      **A2** U1, the plan card, against B's C1.

- [ ] **The pre-chain band tap** (C2 §5.1, Sean approved 27 Oct evening): a second MeterEngine on the V2's
      rack INPUT, fed only inside the `Capturing` branch, with its own hop series on the same 100 ms grid.
      `P_bus` comes from `hopFramesRackIn` when non-empty, else from `hopFrames` with the bus trim added
      back, and `match_report.busTap` says which. Costs a second 2048-point FFT per block while capturing,
      nothing when idle.
- [ ] **C2 §4 payload**: `sharePct` (not normalised), per-band `coverage`, `preFaderCaveat`, `busTap`.
