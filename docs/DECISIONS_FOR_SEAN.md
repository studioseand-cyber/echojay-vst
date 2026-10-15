# MORNING REPORT — 15 Oct, overnight run

**Read this first. Updated as the night goes; the state below is what was true at the last write.**

## WHAT IS INSTALLED

**`ship_2026-10-15a`** — placed, gated green, **installed at 02:46** with Logic closed (checked, not
assumed), and the four arm64 UUIDs read back from `~/Library` match the ship folder byte for byte:

```
EchoJay V2.component   0EB9D09C-206E-3434-8620-6B01EC67B324
EchoJay V2.vst3        A926CB56-1323-33FF-B7A8-40158454BEC6
EchoJay Link.component 483F8ED8-5D57-39A2-9635-B9E70DB056EF
EchoJay Link.vst3      972F32A6-E7F0-3103-BD4B-AA07B5AC9AE7
```

It carries **your seven fixes**, the unknown-track tap choices, and three more the rig found while proving
them. `ship_2026-10-15a/GREEN.txt` is the full record, including every skip and why S6 was re-run.

The gate: S1–S3b OK, **S4 78/78**, S5 OK, **S6 green both ways** (live against B's local server: 47 passed,
0 failed, 1 named skip; the mock: 32 passed, 0 failed, 2 named skips). Commit `947903ff`.

**Nothing else is installed.** Ship 2 did not finish — see WHAT IS LEFT.

## THE PUSH COMMAND

```
git push origin HEAD:release/v2-agent
```

## WHAT IS LEFT

**Ship 2 was not finished, and nothing half-done is installed.** What ship 2 was to contain, and where each
part stands:

| ship 2 item | state |
|---|---|
| **Auto mode** (the Agent chip's third state, the approvals it skips and the ones it keeps) | built against the design and the mockup; see below for how far it got |
| **the plugin-choice card** (P1-P5) | not started |
| **the OUT knob** (O1-O6) | not started - the groundwork is read and written down (§6 of the queue): both racks clamp to -24..+12 dB, a Link's OUT is already ONE value (`gainDb_`), and the V2's own is `busGainDb_` |
| **multi-track jobs** | not started (my half; B's half is in their note) |
| **item 0's surface** (the panel becoming a transcript row) and **0b** (the client, the executor and the two build roads onto the processor) | NOT attempted, deliberately - see the next line |
| **0a** (the deep-meter region + analyser) | not started |

**Why item 0's surface and 0b were not attempted overnight, and it is not the clock.** Their rig legs cannot
pass in this host: the rig **cannot close a window**. Releasing the AU's wrapper view does not destroy the
plugin's editor - one `EchoJayEditor` is built and one destructor runs, at the end of the process - so
"the run survives the window" has nothing to be tested against here, and two older rows (R4 and C11's first
cut) were passing because nothing had died. Ship 1 fixes the instrument as far as it can go (the processor
now writes its own rig line, and every close row names the destroyed-editor half as a skip with its reason),
but **a host that can really close a window is owed before 0b**, and the cheap candidate is loading the VST3
for those rows. Moving a 1,100-line painter and two 1,000-line build methods, unattended, with the legs that
would catch a mistake unable to run, is the one change I was not willing to make while you were asleep.

## LOGIC RECHECK LIST — only what the rig cannot see

**1. Close the V2 window mid-reply, plain chat, on a Link (your check 9).** This is the one the rig cannot
do at all: releasing the AU's wrapper view does **not** destroy the plugin's editor in the rig's host - one
editor is built and one destructor runs, at the end of the process - so the destroyed-window half of fix 6
is unproven here and is named as a skip rather than claimed. Expect, on reopen: either the answer, or a row
reading **"⚠ That answer didn't arrive — … [interrupted]"**. Never silence, and never a spinner that has
stopped with nothing behind it.

**2. "Make the kick punchier" from the Mix Bus (your check 6, the honesty one).** The rig proves the flat
payload now applies and that a `set` reads its values back; what it cannot provide is the real model's
wording. What to watch: the closing sentence must only claim what the read-back confirmed, and a parameter
that did not move must be named as **not applied**.

**3. `check(gr)` with audio playing, on another track's Link.** Fixed and rig-proven on the rack this window
has open (rows G1/G2). On a **Link** it is still refused honestly - that needs item 0a's deep-meter region,
which is not in this ship.

**4. The release banner (your item 4c).** A 1500 ms grace now covers the release-then-re-engage inside one
second that you saw. It needs a human eye on the bus row: select it and watch for a banner that should not
appear.

**5. The 330 s client bound (your item 2).** Only a genuinely slow round shows it, and a slow round cannot be
staged. If one takes a long time now, it should finish rather than die at 60 s, and if it *does* time out the
message says the round was slow - not "check your internet connection".

**6. The Link's view in the main rack straight after an agent build (your item 5).** Rig row K1 asserts the
slot NAMES with no reopen, against the real model. Worth one human pass on the **AI tab**, since that is the
tab whose feeder was the cause.

**7. Undo after a hand pre-gain move (your item 4).** Row U2 covers the move and the press. In Logic, the
case to repeat is the one that bit you: **two presses in a row** must not remove two plugins - the second
should say "nothing left to undo" if the history is spent.

---

# Decisions for Sean — raised overnight, 14 Oct 2026 — ALL FOUR RULED, 14 Oct morning

Sean's rulings came back on all four. Each is recorded here with what it changed, so the file is the record
rather than a question list. Nothing is outstanding.

## 1. The pre-gain double-click restores AUTO — RULED: pre-gain ONLY

> *"keep AUTO on pre-gain only."*

Which is what shipped: `onResetAuto` is assigned by the pre-gain knob alone, so every other bipolar trim still
double-clicks to 0.0 dB and the 21r item 6 ruling of 24 Sep stands everywhere else. **No change needed.**

## 2. A remote slot opens on DOUBLE-click — RULED: keep the double-click

> *"keep double-click for remote slots."*

So the single-tap-to-begin-an-edit-session coupling stays removed. **No change needed.**

## 3. B's duplicate argument names — RULED: drop them once this build is installed

> *"B drops the duplicate names once this build is installed."*

`ship_2026-10-14a` carries the fix (the contract's `wet_pct` / `bypassed` read first). Once Sean has it
installed, B drops `pct` and `on`. In B's note. Until then both spellings are accepted, so neither order of
events can break a call.

## 4. "Undo that" — RULED: YES, undo walks the plugin's undo history one step

> *"YES: \"undo\" walks the plugin's undo history one step; add the undo op and declare the capability (B's
> side is ready and waiting for it)."*

**Implemented 14 Oct** as `{"op":"undo"}` on the `do` tool:

- it walks `EchoJayProcessor::undoHistory` — the one plugin-wide stack that already carries local and remote
  edits in one order — exactly one step, whatever that step was. The model no longer decides what "that" meant.
- it is **read back**: the op reports success only when the history's depth actually moved. A sink that says
  "undone" over a history that did not move is reported as `not_applied`, which is the rule the other nine
  sinks were brought under this round.
- an empty history is a true answer, not a failure of the plugin: *"there was nothing to undo"*.
- an undo whose target has gone (a Link that has left, a slot renamed) is reported as skipped with the reason,
  never applied to a different target — that was already the history's own rule.
- Agent mode off refuses it, because an undo is a change.

Guarded by `agent_round14_guard` N7 (16 assertions, red direction proven) and driven on the rig as row C10.
