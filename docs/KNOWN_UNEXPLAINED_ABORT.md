# Known, bounded, unexplained: SIGABRT in JUCE's quit path

**Status: OPEN and deliberately not fixed.** Recorded 7 August 2026 after a
decision that another hour on it was a bad trade. This is not a closed issue and
it is not a fixed one. If you hit it, this is the honest account.

---

## The signature

```
*** Terminating app due to uncaught exception 'NSInternalInconsistencyException',
    reason: 'Periodic events are already being generated'
  3 AppKit    +[NSEvent startPeriodicEventsAfterDelay:withPeriod:] + 356
  4 ejmap     juce::MessageQueue::deliverNextMessage() + 60
  5 ejmap     juce::MessageQueue::runLoopCallback() + 20
```

The process aborts. `waitpid` sees **SIGABRT (signal 6)**; earlier occurrences
were recorded as signals 5, 10 and 11 as well — an uncaught Objective-C
exception aborts however the runtime happens to, so **the signal number is not a
distinguishing feature.**

It fires **after all the work is finished**: the map is written, the row is
recorded, the sweep summary has printed. That is why it looked, for eleven
deaths, like eleven different plugin faults that left no evidence.

## The mechanism, which IS established

`MessageManager::stopDispatchLoop` ends in `shutdownNSApp`
(`juce_MessageManager_mac.mm:355`):

```objc
[NSApp stop: nil];
[NSEvent startPeriodicEventsAfterDelay: 0  withPeriod: 0.1];
```

The periodic events are a trick to make `nextEventMatchingMask` return so the
stop takes effect. **They are never stopped.** It is the only call to
`startPeriodicEvents` in the entire JUCE tree, it is unbalanced by design, and
AppKit throws if a start arrives while some are already running.
`stopDispatchLoop` sets `quitMessagePosted` but never reads it, so there is no
guard on JUCE's side either.

## What is NOT established: who made the first start

Unknown after fifteen instrumented single-plugin runs against a clone of the
live corpus. Candidates never distinguished:

- a second `quit()` from our own code
- a plugin, or AppKit tracking (menu, drag, autoscroll)
- something in the AU hosting stack

Naming one would be a guess. Three separate wrong causes were named from
adjacency during this week's work; a fourth would be worse than the gap.

## The mitigations, and that they did not prevent it

Both are in the tree. **Neither stopped the 11:44 recurrence.**

1. **`quitNow()` is idempotent** and prints `quitNow: ALREADY QUITTING` when a
   second quit is attempted, so the caller could be identified. *That line has
   never appeared.* Either the double-quit theory is wrong, or the second start
   does not come through `quitNow`.
2. **`ejmap::stopPeriodicEventsIfAny()`** runs immediately before
   `JUCEApplication::quit()`, making JUCE's unbalanced start legal whoever made
   the first one. It did not prevent the abort, which means either the start
   happens *after* this point, or `[NSEvent stopPeriodicEvents]` does not clear
   the state AppKit is testing.

## What the instrumentation would have named, and did not

The sweep's crash-stake protocol covers teardown, calibrate/mask/assign, every
swept index, and submit — each planting `inflight.json` so `recoverFromCrash`
can attribute a death to a plugin and a window.

**On the 11:44 abort it produced nothing.** No `died_during_load`, no
`sweep_timeout`, no session row. The ledger holds only `ok` and `init_failed`
across the whole period.

The plausible reading — *and it is a reading, not a measurement* — is that the
sweep loop had already finished and every stake was legitimately closed, so
there was nothing in flight to name. A death in the quit path is outside every
window the stakes cover, and covering it would mean staking process shutdown
itself.

## The two facts that do not reconcile

1. `endSweep()` deletes `sweep-active.marker` before `quitNow()` is reached, so
   a throw inside the quit path should leave **no marker**.
2. **The sweep resumed after the restart**, which requires the marker to have
   been present.

Both are observed. They cannot both be true of the same instant, so at least one
assumption behind them is wrong — most likely the exact ordering of `endSweep`,
the marker delete and the throw. **Unresolved.** Anyone picking this up should
start here: instrument the marker's existence at the moment of the abort, not
the quit path.

## Why it is not being fixed

Measured cost, on the run that produced this document:

```
supervisor: the sweep finished 152 plugin(s) before this exit; the restart is not charged
supervisor: child died (signal:6) after 1156432ms; consecutive=0 fastDeaths=0 total=0 loadSucceeded=yes
supervisor: relaunching (consecutive 0 of 3, total 0 of 10)
```

**152 plugins banked, restart not charged against the budget, sweep resumed
itself, campaign continued.** The failure is bounded by machinery that already
exists and was proven by this instance: the progress exemption
(`sweepProgressCount` rising ⇒ not charged) and the resume marker.

The trade is an hour or more against a defect that costs one process restart and
no work, and that fifteen instrumented runs could not reproduce on demand.

## What would change that

- It starts costing work — a map lost, or a plugin re-swept because a death
  landed *before* submit rather than after.
- It recurs often enough to eat the supervisor's budget (three no-progress
  restarts) and stop a campaign.
- `quitNow: ALREADY QUITTING` ever appears in a log. That single line converts
  this from unexplained to a one-line fix, and it is already wired.
