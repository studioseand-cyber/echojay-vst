# DEFECT: any post-load crash is unattributed — inflight.json dies at load success

**Filed:** 2026-08-02, during M9 proposal review. Independent of M9; the
window is open today for every post-load activity the tool already ships.

## The code

`tools/ejmap/Source/EjmapLedger.h`, the inflight protocol's own header
comment states the design:

```
beginLoad(id)   -> writes inflight.json
endLoad(id, ok) -> deletes inflight.json, appends a ledger record
```

and `endLoad()` executes it: `inflightFile.deleteFile()` on the success path
(EjmapLedger.h, the first `deleteFile` in `endLoad`). `recoverFromCrash()`
reads `inflight.json` at launch and attributes a dead session to the plugin
named there.

## The consequence

The stake exists only for the duration of the load call. From the moment a
plugin loads successfully until it is unloaded, **a crash inside plugin code
leaves nothing on disk naming the plugin**:

- sweeps (`sweepSetRead` drives plugin parameter code for minutes),
- gesture captures and listener callbacks,
- `applySettings` write-back verify at submit,
- the lockstep write-verify pass at submit,
- editor interaction (the UAD Ampeg hang class was IN this window —
  caught only because the watchdog terminates with its own record),
- every future M9 render.

On relaunch, `recoverFromCrash()` finds no `inflight.json`, no quarantine row
is written, the plugin loads again cleanly, and the crash repeats on the next
identical action. The human's only evidence is macOS's crash reporter.

The watchdog is the accidental exception: a HANG in this window is caught,
because `Watchdog::Scope` sites flush their own record before terminating.
A hard crash (SIGSEGV/SIGBUS in plugin code) has no such record. So today:
post-load hang = attributed, post-load crash = invisible. That asymmetry is
the measurement that exposed this — mpressor's render crashes during M3 were
attributed only because they happened during load-adjacent sweeps driven
interactively, not because the protocol caught them.

## Why it is bigger than M9

M9's proposal adds a probe stake around render batches, which closes the
render window only. The sweep window (M3), the capture window (M2), and the
submit write-back window (M4/M10) all remain open. Seven silent-drop
instances are on this project's record; an unattributed crash is the same
class with a worse costume, because the evidence (a dead process) looks like
the user quit.

## Proposed direction (not built; decision needed)

Keep a stake on disk for the whole time a plugin instance is live:
`beginLoad` writes it (as today), `unload` deletes it, and the record carries
a `stage` field that each activity updates through the existing writeThrough
path (`load` → `idle` → `sweep` → `capture` → `submit_verify` → `probe`).
Relaunch attribution then names both the plugin and the activity.

Trade-off to decide, not assume: a crash NOT caused by the loaded plugin
(ejmap's own bug, or the OS) would be blamed on whatever was loaded. That is
what the `stage` field and the existing quarantine-release path are for, and
it is the same trade the load-time protocol already accepts for the load
window. The alternative — per-activity stakes written and deleted around
each risky call — is more precise and more code, and every new activity must
remember to plant one (the class of defect that forgetting produces is
exactly this defect).

## Status

Documented only. No code changed. Blast radius: every post-load crash since
the tool existed; frequency unknown precisely because the class is invisible.
