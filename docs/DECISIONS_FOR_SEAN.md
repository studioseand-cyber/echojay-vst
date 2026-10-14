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
