# EchoJay Agent: Scenario Matrix and Test Loop

**For:** Session A (plugin, `~/echojay-vst`) and Session B (server, `~/echojay-saas`), working together.
**Goal:** stop finding agent bugs one at a time by hand in Logic. Every path the agent can take is listed here as a scenario with checks. The rig runs all of them against real installed plugins and the real model, failures get fixed, and the loop repeats until the matrix is green. Sean only looks at what a machine cannot judge.

Save as `docs/AGENT_SCENARIO_MATRIX.md` in both repos. A owns the runner and the plugin-side checks; B owns the server/model-side checks and the live-model runs. Each failure is triaged to one owner with evidence.

---

## 1. Why this exists

On 15-16 Oct the gate passed while Sean's Logic recheck failed on ~22 items. The rig had three blind spots:

1. It only loaded EchoJay's own devices, never a real third-party plugin.
2. It used a scripted model, so it never saw the real model contradict a result.
3. It asserted internal state, not what the window actually draws.

This matrix closes all three. A scenario is only "covered" if it runs on a real plugin instance, through the real server and model (live pass), and asserts on what is drawn on screen.

---

## 2. Dimensions

Every scenario is one combination of these. Not every combination is listed; section 5 picks the ones that matter, plus pairwise coverage.

| Dimension | Values |
|---|---|
| **Target** | T1 own rack (Mix Bus V2) · T2 a Link (Kick) · T3 a Link whose rack is selected/locked in another instance ("Selected in the rack on ... deselect there") · T4 two Links in one job |
| **Plugin kind** | P1 EchoJay built-in (EQ, Compressor, Limiter, Gate) · P2 third-party, good map (Renaissance Axx, Vertigo VSM-3) · P3 third-party, map exists but wrong (MTransient today) · P4 third-party, no map · P5 fails to load (kHs Transient Shaper) · P6 loads but expired eval/demo (Pro-C 3, MTransient) · P7 does not exist on this Mac ("Pro-C 2") · P8 same plugin present twice (AU + VST3 / alias) |
| **Operation** | O1 add · O2 add with settings · O3 set on an existing slot · O4 replace · O5 remove · O6 reorder/move · O7 build a chain · O8 pre-gain set · O9 undo one step · O10 undo all · O11 check(gr) · O12 look at rack / read-only question · O13 non-chain question (project name, track names) · O14 level match |
| **Agent mode** | M0 Off (classic chat) · M1 Ask · M2 Auto |
| **User path** | U1 approve with the line's Apply · U2 Apply all · U3 Skip / Skip all · U4 tap a choice chip · U5 type an answer to an ask · U6 type a NEW request while an ask is waiting · U7 press Stop mid-run · U8 close the window mid-reply (M0) · U9 close the window mid-run (M1/M2) · U10 reopen and Retry · U11 scroll up / re-render · U12 switch rack mid-run |
| **Audio** | A1 playing loud material · A2 transport stopped · A3 playing but the slot is silent/muted |

---

## 3. Invariants: checked on EVERY scenario

A scenario passes only if all of these hold. Each one is an assertion in the runner, not a human judgement.

| ID | Invariant | How it is checked |
|---|---|---|
| **I1** | **State truth.** Every value the closing or a step names equals the value actually on the hosted plugin, within tolerance. | Read each named parameter from the plugin instance itself (value AND its display text) after the run. Third-party parameters too. |
| **I2** | **Closing matches result.** Never claims applied / confirmed / a figure / a position / a GR number that the result and the real rack do not show. | Compare closing text against the ack and the actual slot order. Server check (B) plus rig check (A). |
| **I3** | **Rendered once, in order.** Each user message, chip answer, step and closing appears exactly once, in chronological order, including after close/reopen, Retry, and scroll/re-render. | Assert on what the window draws (onScreen), not the stored transcript. |
| **I4** | **Clean wording.** No raw parameter IDs (`band1.freq_hz`), no text addressed to the model ("Report it as ADDED ... Do NOT"), no internal tags (`[interrupted]`), no raw provider errors. | Regex deny-list over every drawn string. |
| **I5** | **Asks are answerable.** An ask always draws its choices AND accepts a typed reply. A typed reply is never silently dropped. A run never ends on a question nothing can change. | Assert chips drawn; send typed reply; assert it lands in the ask. |
| **I6** | **One approval surface.** One pending line shows only that line's buttons; two or more add "Apply all / Skip all". A tapped chip IS the approval. | Count drawn approval buttons per state. |
| **I7** | **Rack view is current.** The main plugin's rack strip shows the real rack (including a Link's) within 3 s of any change, with no manual rack switch. | Compare drawn slots with the actual rack. |
| **I8** | **GR is honest.** Tile GR equals the device meter's held value. GR figures only for EchoJay devices and certified compressors. No GR on uncertified plugins or on a Link mirror (until Link meters ship). check(gr) on an uncertified slot returns not_certified immediately, never "no audio". | Read meter value, tile value and check(gr) result in the same block. |
| **I9** | **Suggested Settings tells the truth.** It shows read-back values, or labels values as "sent, not confirmed". | Compare panel text with read-back. |
| **I10** | **Undo is exact.** Undo restores value AND hand-set/auto state; the text after an undo describes the undone state, not the old change. | Read state before/after. |
| **I11** | **Read-only runs change nothing.** A run that only read or listened shows no "N changes" and no Undo all. | Assert label. |
| **I12** | **Budget.** Single add: built-in ≤ 2 rounds, third-party ≤ 4 rounds. Whole run under 60 s unless it listens. | Count rounds/time. |
| **I13** | **Run order.** Listen before measuring; add before set; nothing is measured on a Link where it cannot be. | Assert step order. |
| **I14** | **New request wins.** A new instruction typed while a run waits closes that run (marked stopped) and starts a fresh run. | Assert two runs, first stopped. |

---

## 4. Plugin test set (on Sean's Mac)

The runner uses real installed plugins. Pick plugins that do NOT pop a blocking dialog in the rig, except where the dialog is the case under test.

| Role in tests | Plugin | Notes |
|---|---|---|
| P1 built-ins | EchoJay EQ, Compressor, Limiter, Gate | control case, must always pass |
| P2 good map, dials | Renaissance Axx, Vertigo VSM-3 | proven to dial on 16 Oct; the read-back proof case |
| P3 wrong map | MTransient | map treats it as a compressor; stays as a regression case after B fixes it |
| P4 no map | pick one installed plugin with no map | must say "set it by ear", never a figure |
| P5 fails to load | kHs Transient Shaper | must not be offered; if forced, clear reason + Details |
| P6 expired eval | Pro-C 3, MTransient | allowed if it loads (Sean's call); read-back must still be truthful |
| P7 not installed | "Pro-C 2" | agent offers the nearest real match |
| P8 duplicate entries | Pro-C 3 / FabFilter Pro-C 3 | one chip only, the mapped one |

---

## 5. Scenarios

Format: **ID** · setup · input · expected (beyond the invariants, which always apply). Each of Sean's 15-16 Oct findings is marked ★ with its item number, and must exist as a scenario that ran RED on ship_2026-10-16a before its fix.

### A. Add and dial

- **A1** T1 P1 O2 M1 · "Add the EchoJay Compressor with threshold at -20 dB and ratio 4:1" · added, both values read back and named.
- **A2** T2 P1 O2 M1 · same on the Kick's Link · same; main rack updates (★12).
- **A3** T1 P2 O2 M1 · "Add Renaissance Axx with threshold at -20 dB" · dials AND reads back -20 as confirmed (★ read-back, 16 Oct).
- **A4** T2 P2 O2 M1 · same on the Kick's Link · confirmed; Link's Suggested Settings populated (★9 correction).
- **A5** T1 P3 O2 M1 · "Add MTransient with attack +6 dB" · until map fixed: "Added MTransient. Settings not confirmed." After B's map fix: dials Attack +6 and confirms.
- **A6** T1 P4 O2 M1 · add with a setting · "Added X. You'll need to set it by ear." No figure.
- **A7** T1 P6 O2 M1 · Pro-C 3 with threshold -18 · truthful either way; if the eval blocks changes, says so in plain words (★20).
- **A8** T1 P7 O1 M1 · "Add Pro-C 2" · offers the real nearest match (Pro-C 3) as a chip.
- **A9** T1 P8 O1 M1 · "Add Pro-C 3" · one chip, not two (★ B-c).
- **A10** T1 P5 O1 M1 · force "Add kHs Transient Shaper" · fails with the real reason; run stays open with Details (★11b).
- **A11** T2 P2 O2 M2 · Auto mode add with settings · no approval asked for a small move; add on another track still asks per the mode table.
- **A12** T1 P1 O7 M1 · "make the kick punchier" · builds or offers a choice; every slot's settings read back; step details in plain words (★6 morning).

### B. Choices, asks and replies

- **B1** T2 · "Add a transient shaper to the kick with attack at +6 dB" M1 · ask draws chips (★ blocker) and accepts a typed "MTransient".
- **B2** same · tap a chip (U4) · added with no further Apply (★ tap).
- **B3** same · type an answer (U5) · lands in the ask, run continues the same job.
- **B4** an ask waiting · type a new request (U6) · old run marked stopped, new run starts (★21).
- **B5** an ask waiting · press Stop (U7) · run stopped; "Hide steps" works on the stopped run (★2, ★13).
- **B6** any run whose result cannot change by asking · run ends without a question (★7 B).
- **B7** "Sort out the low end" M1 · asks if needed, continues with the answer.

### C. Honesty of the closing (live model, run each ×10)

- **C1** add whose read-back is empty · closing = "Added X. Settings not confirmed." Never "Not applied: add X", never "running on default values" (★4/★5).
- **C2** add placed before another slot · position stated correctly (★ B-b; 16 Oct "after Renaissance Axx" was wrong).
- **C3** EQ set the plugin could not confirm · closing never says "confirmed" (★ Test 9).
- **C4** model tries to describe its plan · server rewrites to the result.
- **C5** after Undo all · text describes the undone state (★14).

### D. Undo

- **D1** hand-set pre-gain -5 → agent sets 0 → Undo · back to -5, hand-set (U1 ruling).
- **D2** pre-gain on auto → agent sets → Undo · back to auto.
- **D3** add with settings → Undo · slot gone, rack as before.
- **D4** two-step run → Undo one step · only that step reverts.

### E. Gain reduction

- **E1** T1 P1 A1 · "how much gain reduction is the compressor doing?" · loudest hits (95th percentile) and typical, both near the meter; no instant max quoted (★17).
- **E2** T1 P1 A1 · tile vs meter · same held number; caption fits, not "WORK..." (★16).
- **E3** T1 P2 A1 · same question on Renaissance Axx · "This compressor hasn't been tested with EchoJay yet..." immediately, no measurement, never "no audio" (★19).
- **E4** T2 P1 A1 · on a Link · agent does not call check(gr); says plainly it can't read GR on a Link yet; no "Try once more" (★8 morning).
- **E5** T2 tile · no GR on a Link slot until Link meters ship (★7 morning).
- **E6** T1 P1 A2 · transport stopped · asks to play once, then measures; listens before reading (★17 order).
- **E7** GR meter label · held figure never overlaps another label (★15).
- **E8** read-only GR run · no "N changes" (★18).

### F. Window, reopen and order

- **F1** M0 · ask, close window within 2 s, reopen · reply, or "You closed the window before the answer came back." once, with Retry (★7, ★9 morning).
- **F2** F1 then Retry · answers the right question; message order stays chronological (★ Test 9 order).
- **F3** M1 run, close window mid-run · stops as 15b (0b off) and says so; no duplicate lines on reopen.
- **F4** scroll up after any run · still no duplicates (★3).

### G. Rack view and layout

- **G1** agent adds to a Link (T2) · main rack strip updates without switching (★3 overnight, ★12).
- **G2** agent builds on a Link · same.
- **G3** mode chip clicked through Off/Ask/Auto · hint shows ~3 s above composer, tooltip on hover, never over Suggested Settings (★8 overnight).
- **G4** one pending line · one set of approval buttons (★4 morning).
- **G5** no stray status labels ("applied" on the right of status lines) (★5 morning).
- **G6** no separate "Chain built - N plugins loaded." bubble beside a run message (★3 morning).
- **G7** closing appears only inside the run message, not also as a separate bubble (★11).

### H. Non-chain questions

- **H1** "What project is this?" · real project name.
- **H2** names with underscores · literal, no italics.
- **H3** Agent Off, "Add MTransient to the kick" · adds just that plugin or says "Turn the agent on for single changes"; no build brief (★ B Agent Off).
- **H4** Agent Off build with explicit threshold · explicit figure wins over auto-match (★5 B).

---

## 6. How a run works

1. **Two passes per scenario.** A **mock pass** (scripted model) for deterministic plugin logic, and a **live pass** (real server, real model). Live scenarios run ×5; the honesty scenarios in section C run ×10. Honesty invariants (I1, I2) must be 10/10. Others: 5/5.
2. **Real plugin instances.** Third-party scenarios load the real installed plugin in the rig host (VST3 road where window close matters).
3. **Assert on the screen.** Every text check reads what the editor draws.
4. **Evidence per scenario:** a PNG of the editor's final state (chat + rack), the drawn text, the plugin's actual parameter values, round count and timing. Stored under `docs/review/<date>/<scenario>/`.

## 7. The loop (until green)

1. Run the full matrix on the current build. Produce `MATRIX_REPORT_<date>.md`: one row per scenario, pass/fail, owner (A or B) for each failure, and the evidence.
2. Each owner fixes its failures. A fix only counts when its scenario goes RED to GREEN.
3. Rerun the failed scenarios ×5 (×10 for section C), then the full matrix as regression.
4. Repeat 1-3. **Do not run a full gate or install until the matrix is green.** Then gate once and install.
5. Anything that genuinely cannot be fixed this round is moved to a QUEUED list with its reason. It is never skipped silently and never counted as a pass.

## 8. What Sean still does

- **Only the "needs your eyes" list.** The runner writes `NEEDS_SEAN.md`: scenarios where the check can't decide (does it look right, does it sound right), each with its PNG and one line on what to judge. Sean answers those, not a full recheck.
- **A short Logic smoke test** (about 10 minutes) after install, because Logic's AU hosting has behaviour the rig cannot reproduce (window close on the AU road). The list comes from the matrix, not from memory.
- **Decisions.** Any product call goes at the top of `NEEDS_SEAN.md`. Don't stop the loop for it: take the most reasonable reading, record it, carry on.

## 9. Rules for both sessions

- Logic closed while the runner or gate runs.
- B never touches env, secrets or KV; FORGE_DEV_OPEN only in the local file.
- B pushes and deploys within 20 minutes of each other, or holds the push. Live-model runs use a local or preview server, not production.
- No em-dashes in anything user-facing.
- Never count a skipped scenario as a pass.
