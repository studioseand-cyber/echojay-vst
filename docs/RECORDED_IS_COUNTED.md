# Recorded is counted, and the numbers answer the question they are read as

**Built 10 Aug 2026 against `eb098bf` (feat/ejmap), in `ejmap-wt`.** Two causes
diagnosed on Mac 2 the night before and not built. Both are built here; the
Mac 2 re-run from a fresh ledger is the test neither of them has passed yet.

Per the stale-branch filing rule: the code described below EXISTS at `eb098bf`
and was read there before anything was written.

---

## CAUSE A — a row that was written was not a row that counted

### What was measured, before anything was built

The live ledger, `~/Library/ejmap/ledger.json`: **144,911 rows, 54 MB**.

| outcome | rows |
|---|---|
| ok | 140,468 |
| no_types | 4,249 |
| timeout | 65 |
| init_failed | 59 |
| crash_on_load | 28 |
| restarted | 26 |
| died_during_load | 13 |
| no_params | 3 |

Every failure path wrote its row. Only two branches ever counted one:

- `recoverFromCrash` — a death recovered from an orphaned stake. **13 rows.**
- `recordWatchdogExpiry` — and it counted `countPriorOutcomeLocked (id,
  "timeout")`, one literal outcome string.

`endLoad` is where an ordinary failed load lands, and it appended the row and
decided nothing. So did `appendRow`.

**The live instance is `bloom`** (`AudioUnit:Effects/aufx,BlmA,OekS`):
eighteen `init_failed` rows between 7 Aug 10:38 and 7 Aug 15:15, plus a death,
and it was still being offered by the worklist on **10 Aug 11:56**. Nineteen
pieces of evidence about one plugin, in a file the decision did not read.

Five plugin/stage pairs were already at or over the threshold and unquarantined:
bloom (19), Weiss Deess (8), H-EQ (s) (4), Bass Rider (m) (4), SPL HawkEye (3).
Weiss Deess is the case the single-string count could never reach: **five
timeouts and three init_failed**, so no one outcome ever accumulated.

### The proof, run BEFORE the fix

`testRecordedIsCounted()` in `ejmap-roundtrip-test`. Ten ordinary failed loads
for one plugin id, each in its own `Ledger` over a throwaway root — ten separate
launches, as they look from disk. No crash, no orphaned stake, no watchdog.

Run against unfixed `eb098bf`:

```
FAIL: counted: THREE RECORDED LOAD FAILURES QUARANTINE
FAIL: counted: and it stays quarantined at row 4
FAIL: counted: ten failures do not un-quarantine it either
2730 checks, 3 failures
```

Rows 1 and 2 correctly did not quarantine. Row 3 did not either, nor row 10.
**The diagnosis was right and the fix was aimed at something real.**

### What was built

The count and the decision moved into **`appendLocked`**, which all four
writers already funnel through. `recoverFromCrash` and `recordWatchdogExpiry`
lost theirs. A path added later cannot forget to count, because there is
nowhere else to write from.

Three things the diagnosis flagged, each resolved:

1. **`isQuarantinedLocked`** added. But the stated reason was wrong:
   `juce::CriticalSection` is a RECURSIVE pthread mutex
   (`juce_SharedCode_posix.h`, `PTHREAD_MUTEX_RECURSIVE`), so calling the
   public `isQuarantined` from a lock-held path would **not** have deadlocked.
   The variant is right as hygiene — and the day someone swaps this for a
   SpinLock, the re-entrant call becomes a hang with no other warning — but it
   was not fixing a live hazard.

2. **The count spans the failure family**, not one outcome string.
   `countPriorOutcomeLocked` and `countPriorCrashesLocked` are **deleted**, not
   left unused: a helper that answers a narrower question than the one it will
   be asked is the defect, kept.

3. **The cost was measured, at full size**, and is a permanent gate
   (`testCountingCost`, on a 57 MB / 144,911-row fixture):

   ```
   one full pass 723 ms | 10 failed loads 1,476 ms total (~2.0 passes, not 10)
                        | 100 ok rows 21 ms total (0.21 ms each, no pass)
   ```

   A naive per-row read would have been ~24 hours per sweep. The file is read
   **once per launch** (a lazily-built per-`(binary, stage)` tally, seeded from
   disk, incremented as rows are written — `appendLocked` is the only writer
   and holds the lock) and **once more per quarantine** (the full
   `RetryEvidence` that the operator reads). A successful load reads nothing.

### Which outcomes count, and the exclusions that matter more

Counted: `died_during_load` / `crash_on_load`, `timeout`, `sweep_timeout`,
`init_failed`, `no_params`.

Not counted, each for a stated reason:

| outcome | why not |
|---|---|
| `no_types` | 4,249 rows, all at stage scan. A fact about the FILE, re-read every scan; nothing was attempted. Counting it quarantines ~300 bundles, and `PluginScanner` then stops describing them at all. |
| `license_refused` | authorisation is a fact about the MACHINE. `PluginHost.cpp:109` already records this decision. An unplugged iLok would otherwise withdraw a vendor's whole catalogue overnight, manual to undo. |
| `no_editor` | the plugin LOADED. The sweep runs headless by default and maps it fine. |
| `restarted` | a supervisor event at stage `session`. Not an attempt. |
| `quarantined` | the consequence, not the evidence — it would let a quarantine confirm itself. |

**The two measured thresholds survived the move** and are asserted:
a scan hang quarantines on the FIRST timeout (it blocks every bundle behind it
— TDR SlickEQ M and Solid Dynamics each cost a 4.5-minute rescan); any other
hang gets exactly one retry (Cymatics Lotus instantiates in 407 ms and was
quarantined by a deadline that was too tight); everything else is
`kRetryAttempts` = 3.

### Broken on purpose, and the gate caught each one

- unfixed code → the three `counted:` proofs fail
- "count everything" (adding `no_types`, `license_refused`, `no_editor`) → the
  three `excluded:` proofs fail
- thresholds regularised to a flat N=3 → the two `thresholds:` proofs fail

**2,744 checks / 0 failures** with all three reverted. `audit_maps` clean over
1,108 maps. The watchdog test's scan hang now writes
`[attempt 1 of 1 at stage scan: quarantined]` and — new, and free — its
quarantine entry carries full `RetryEvidence`, which it never did before.

### A SUCCESS SETTLES A NON-DEATH FAILURE — found after the above was green

Found while working out what to do about the plugins already past the
threshold, which is the only reason it was found at all. **H-EQ (s) has four
load failures on 4 Aug and SIX successful loads on 9 Aug.** The count was
cumulative over the ledger's whole life and nothing ever reset it, so a working
plugin sat at 4 against a threshold of 3 and would have been withdrawn on its
next failure.

It does not stop at one plugin. The ledger only grows. Every binary in the
catalogue would eventually accumulate three failures and be withdrawn for
having been used. With deaths alone (13 rows in 144,911) it never bit;
**counting ordinary failures is what turned a dormant flaw into a slow fault**,
and shipping the fix without this would have been a worse defect than the one
it fixed.

The rule now: **non-death failures count only since the last SUCCESS at that
stage.** The process survived each one to report it, and a later successful
load is direct evidence that the cause is not a permanent property of the
binary. `otherFailures` keeps the lifetime figure for the record;
`otherFailuresSinceOk` is what the decision reads.

**A death is deliberately NOT settled.** That is the signed rule from the retry
work — "three deaths still quarantine even with a success between them" — and
the nightly non-deterministic re-test is the escape built for it. The obvious
tidy-up is to let a success reset everything; that would silently repeal a
decision made on measured evidence, so both directions are pinned by tests and
both were broken on purpose to prove the gate holds them.

### NOT retroactive, and this is deliberate

Quarantine happens on an append, not at startup. bloom sits at 19 counted
failures against a threshold of 3 and is **still not quarantined**; it is
withdrawn the next time it fails. The alternative — sweeping the ledger at
launch and withdrawing everything already over the bar — would apply a rule
written today to 144,911 rows written under other rules, without a human
seeing it happen. On a fresh Mac 2 ledger it costs nothing.

### The Mac 1 plugins already past the bar, measured one at a time

Five looked past the threshold. After the settle rule and after counting real
events rather than rows, **two are**:

| plugin | rows | real events | counted now | reading |
|---|---|---|---|---|
| bloom | 20 | 20 | **19** | 0 successes ever; 18 init_failed, one death that took the process down. Genuine. |
| Weiss Deess | 9 | 9 | **8** | 0 successes ever; 5 hangs + 3 init_failed across 2/7/10 Aug, incl. a sweep death. Genuine. |
| H-EQ (s) | 4 | 2 | **0** | six successful loads on 9 Aug settled it. Nothing to do. |
| Bass Rider (m) | 4 | **2** | 4 | two events written twice, 1 ms apart. Waves -10875, clears on restart. |
| SPL HawkEye | 3 | **2** | 3 | one duplicated pair. `no_params` — nothing on it to map. |

**The duplicate inflation is real and is NOT corrected.** `endLoad` has
de-duplicated same-plugin/same-outcome closes since 5 Aug, but the historic
rows remain and the pairs are 1 ms apart, not identical — so there is no
reliable discriminator recoverable from a row alone, and inventing a
near-timestamp heuristic to rewrite how history is counted would be a worse
trade than the two plugins it protects. Recorded here instead: **Bass Rider (m)
and SPL HawkEye will be withdrawn one real failure earlier than the rule
intends.** Release is one command and the reason string will name the count.

---

## CAUSE B — the numbers answered a different question than the reader's

### What it said, and what it should have said

`SWEEP WOULD OPEN` counted **worklist rows** and applied none of the sweep's
own filters. The worklist answers *"what is unmapped?"*; the button is asked
*"what will happen tonight?"*. On Mac 2 those were 1,460 and 40.

Measured on Mac 1 after the fix:

```
SWEEP WOULD OPEN 2 of 706 worklist row(s) (706 unmapped, 0 parked, 0 unqueried);
the sweep itself declines 704 (445 no dial set, 95 not a processor,
8 quarantined, 156 review). Not on the worklist at all: 1110 already mapped,
0 unmappable, 3 flagged
```

**706 → 2.** Both numbers are kept, because the gap between them is itself the
information: it is how much of the catalogue is waiting on a *category* rather
than on a night's work.

### One cascade, and nothing re-implements it

`MainComponent::decideSweep (sp, cats)` is the authority — category lookup,
sweepable, quarantine, in that order, unchanged from the run's own. Quarantine
stays LAST on purpose: an uncategorised plugin that is also quarantined reports
as uncategorised, because the operator's next action follows from the first
reason, not the last.

`sweepStep`, the on-screen banner and `beginSweep`'s opening line all call it.
The dry run already sat behind the cascade and now reports through the same
projection.

**Why there is no unit test for this cascade.** `RoundTripTest.cpp` cannot
instantiate `MainComponent`, so such a test would re-implement the cascade in a
lambda — a THIRD implementation, guarding two others by agreeing with neither.
That is the defect being fixed, wearing a test's clothes.

The guard is at RUN TIME and is better: `endSweep` reconciles the forecast
against what the run actually did, on the real catalogue, every run. It earned
itself immediately — it caught a genuine error in **its own first version**,
where a dry run reported `forecast 2, actual 0` because a dry run opens a
plugin without recording an outcome for it.

```
forecast 2 to open, 704 to decline  |  actual 2 opened, 704 declined   (agree)
```

A disagreement is not automatically a defect — a binary quarantined by its own
failures mid-run was forecast openable and is then skipped, which is the retry
rule working — so the line names the gap rather than asserting a cause.

### Per-run reporting: `--sweep-report [--all]`

A supervised sweep is many runs — **51** on this machine — and every summary
before this described whichever child went last. Adding the per-run logs is
worse than useless: the six relaunches of 7 Aug 12:01–12:12 each declined the
same 321 rows and mapped nothing.

So the report gives ROWS (what the runs did) and DISTINCT PLUGINS (what the
catalogue contains), and their ratio:

```
     outcome                      rows  plugins  re-seen
     init_failed                    30        3    10.0x
     load_failed                    29       20     1.4x
     mapped                       1527     1069     1.4x
     skipped_no_dial_set          3158      445     7.1x
     skipped_not_a_processor       709       94     7.5x
     skipped_quarantined            71        8     8.9x
     skipped_review               1504      156     9.6x
```

`init_failed` at **10.0x** is Cause A in one number: three plugins, thirty
attempts. Under the fix those thirty become nine.

`endSweep` also now names its run id and says it describes one run only.

### The 236 skipped_uncategorised: settled as a method, not yet as a number

**Mac 1 has zero.** Across all 51 runs, no `skipped_uncategorised` row has ever
been written here — every worklist row had a category entry when it was
reached. So 236 is a Mac 2 number and cannot be adjudicated from this machine.

What the report will settle there, and prints explicitly:

- 236 rows over 236 distinct plugins → 236 real gaps, and the answer is
  Categorise.
- 236 rows over a handful of plugins → a few gaps re-skipped across relaunches,
  and the count was never the problem.

Given every other skip class on Mac 1 runs at 7–10x re-seen, the second is the
likelier shape — **but that is a prediction from a different machine and is
recorded as one.**

---

## What is NOT done

- **The Mac 2 re-run from a fresh ledger, unattended, with no hand exclusions.**
  Neither cause has faced it. It is the only test that matters and it is the
  user's next step.
- The `--sweep-report` uncategorised verdict is unexercised: this machine has
  no rows of that class to exercise it with.
- `no_params` counting withdraws SPL HawkEye on its next failure. That is
  intended — there is nothing on it to map — but "quarantine" is a word meaning
  *dangerous*, and a plugin with no automatable parameters is not dangerous, it
  is empty. The reason string says which. If the vocabulary matters, that is a
  marks question, not a ledger one.
