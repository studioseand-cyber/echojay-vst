# Queued: the refusal-message audit

**Status:** not started. Queued 4 Aug 2026, after the controls-gate defect.

## The rule being enforced

**A refusal states what it observed, never why.** If a message names a cause, that cause
must have been measured in the same code path. Otherwise it is a guess wearing the
authority of a diagnostic, and it will be believed.

## Why this is a milestone and not a grep

The submit gate refused with:

> controls row claims '0 named control(s)' but 0 controls are staged **(session restore
> lost them)**

The parenthetical was never checked. The real observation — claim 0, store 0 — is
agreement. Cost: a ledger backup, two re-sweeps of a correct row, and a session spent
hunting a restore bug that did not exist. Both the user and the assistant acted on the
asserted cause without questioning it, because it read as a finding.

A keyword sweep was already run (`lost`, `because`, `stale`, `crashed`, `must have`,
`probably`, `restore`) across `conflicts.add` / `refusedReason` / refusal sites. It
surfaced only comments. **That sweep is not the audit and must not be mistaken for one:**
it finds messages phrased in the words someone thought to search for, and misses
confident narration that uses none of them. The Cenozoix message would have been caught
only because it happened to contain "restore".

## What the audit is

Read EVERY user-facing refusal, guard, conflict and assertion message — not grep them —
and for each ask:

1. What does this message CLAIM? (observation, cause, or both)
2. What did this code path actually MEASURE before emitting it?
3. If (1) exceeds (2), rewrite to the observation and stop.

Sites to cover, at minimum:

- `EjmapAssignPanel.h` — `conflicts.add` (submit gate), skip/resolve reasons, `say()`
- `MainComponent.h` — gate suites' verdict text, quarantine reasons, scan errors
- `EjmapSend.h` / `.mm` — `refusedReason` (NOTE: the timeout text is CORRECT — it states
  an unknown rather than claiming one, and says so explicitly. Keep it as the model.)
- `EjmapLedger.h` — quarantine rows and their recorded causes
- `EjmapTriage.h` / `EjmapProbeRoute.h` — `routeText` and liveness classification

## Known-good example to copy

`EjmapSend.h`'s timeout: *"the server may or may not have received it, so this is REFUSED
rather than unknown and must not be retried without checking the server first."* It names
the ambiguity instead of resolving it by assertion.

## Related

- The same `if` also collapsed two states into one refusal — fixed 4 Aug 2026, commit
  covering `EjmapAssignPanel.h` gate arms (controls + bands).
- The drift gate does NOT cover these messages. Anything changed here is unproven by it.
