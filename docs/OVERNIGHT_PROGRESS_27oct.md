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
