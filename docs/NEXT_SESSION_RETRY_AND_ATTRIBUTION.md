# Next session: the retry rule and force-quit attribution

**Signed 4 August 2026. Nothing else that session.**

Both items are decided. Nothing here needs redesigning — it needs building and
proving.

---

## 1. The retry rule

**N = 3.** Not chosen; computed from the ledger (7,055 records, 917 plugins).

**Nine plugins have failed a load AND succeeded at one** — the population a
single-crash rule loses:

| rate | record | plugin |
|---|---|---|
| 33.3% | 1/3 | Solid EQ |
| **32.4%** | **11/34** | **SSL X-Gate** — the gate suite's own subject |
| 20.0% | 1/5 | CLA-76 (m) |
| 14.8% | 4/27 | **bx_limiter True Peak** — the limiter suite's subject |
| 12.5% | 1/8 | Solid Dynamics |
| 10.0% | 1/10 | SSL SubGen, ANA2 |
| 6.2% | 1/16 | API-2500 (s) |
| 3.3% | 1/30 | CLA-76 (s) |

Against the worst observed rate of 33.3%:

| N | false-quarantine probability |
|---|---|
| 1 | 33.3% (today) |
| 2 | 11.1% |
| **3** | **3.7%** |
| 4 | 1.2% |

N=3 is the knee. N=4 buys 2.5 points and costs a third more load time on the
~350 plugins that always fail.

**Three of the nine are M9's own signed subjects.** Today's rule would have
quarantined the tool's fixtures.

### Requirements

- **Separate process launches**, through the supervisor. A process that has
  taken a SIGSEGV inside plugin code is not a sound place to retry from, so the
  attempt counter is persistent on disk and the supervisor relaunch is what
  makes attempts independent.
- **The quarantine row carries**: `attempts`, `failures`, `outcomes[]`,
  `load_ms[]`, `prior_ok_in_ledger`, `prior_ok_this_session`.
- **The NON-DETERMINISTIC note** when prior successes exist:
  > "this plugin has loaded successfully N times before, once in this session.
  > A quarantine here is a single bad roll, not a verdict on the plugin."
  The ledger already holds every number; nothing new is recorded, only read at
  quarantine time. It changes the operator's action: `prior_ok: 0` means do not
  bother, `prior_ok: 4` means retry.
- **Nightly re-test** of quarantined plugins with prior successes. Release on
  binary change alone leaves nine plugins waiting on a change that may never
  come.

### BUILT 5 Aug 2026

`EjmapLedger.h`: `RetryEvidence`, `retryEvidenceForLocked` (one pass, stage- and
binary-scoped), `kRetryAttempts = 3`, the rewritten decision in
`recoverFromCrash`, and `nonDeterministicQuarantine()` for the nightly re-test.
`crash_on_load` is now `died_during_load` with a `certainty` field; both
spellings count, because the 28 historic rows are the only determinism evidence
this project has.

Operator surface: `--retry-evidence <plugin_id> [stage]` reads the numbers out;
`--retest-nondeterministic [--apply]` selects the release population, dry run by
default.

`load_ms` is timed **from the inflight stake** inside `endLoad`, not from a
caller's stopwatch: beginLoad is written immediately before control passes to
plugin code, so the interval is exactly what "was this a slow load?" is asking,
and none of the 20+ call sites had to be touched.

### The proof, required before it lands

1. Force a load failure and confirm **three separate launches** before quarantine.
2. Confirm a plugin **succeeding on attempt two is never quarantined**.

A quarantine change that ships unproven is worse than today's rule, which is
wrong but predictable: a half-tested retry could fail to quarantine something
that genuinely crashes every time.

**Both proved, in `ejmap-roundtrip-test` (the always-on pre-commit gate), 477
checks.** A launch is a fresh `Ledger` over the same root, which is exactly the
requirement: it reads the counters off disk with a new run id and holds nothing
in memory. Six more cases ride with them — the binary-keying, the stage-scoping,
the unattributed discount, and that historic `crash_on_load` rows still count.

Each rule was then **broken on purpose** and the gate caught it: `N=1` fails the
two signed proofs; an unscoped `prior_ok` fails the stage case; keying on the
display name fails ten checks.

The crash-report lookup reads a directory the test process does not own, so it
is substituted via `Ledger::testOnlyCrashOverride`. Without that seam every
simulated death records as unattributed and nothing ever quarantines — the test
would have passed by proving the opposite of what it claims.

### Where it lives

`EjmapLedger.h:407` `quarantine()`, `recoverFromCrash()`, and
`MainComponent.h` `loadSelected()`.

---

## 2. Force-quit attribution

**A SIGKILL and a SIGSEGV leave identical evidence**: a stake with no
completion. `recoverFromCrash()` sees `inflight.json` and nothing else. They
cannot be fully separated, so the fix is to stop asserting a cause.

- `crash_on_load` becomes **`died_during_load`**, with
  `certainty: "unattributed"`.
- Check `~/Library/Logs/DiagnosticReports/` for a report within **10 s** of the
  stake's timestamp. A real crash writes one; a SIGKILL does not.
- **Absence is weak evidence** — reports can be delayed or disabled — so it is
  recorded as weak in the row rather than resolved:
  > "no crash report was found within 10s of the stake. This may be an operator
  > kill of a slow load, not a plugin fault."
- Corroborated by a report: the row may say `crash_on_load` with
  `certainty: "corroborated"`.

Same discipline as `noMap` versus `unmapped`: name what was observed, not what
it probably means.

### The rule that joins the two items

**An unattributed death does not count toward the three.**  *(Built. The hole it
leaves is real and stated: a machine with crash reporting disabled would never
quarantine anything. Measured here, 26 of 28 death rows found a report, so the
discount applies to ~7% of them — and the ledger row NAMES the uncounted deaths
rather than hiding them, so a plugin dying unattributed over and over is visible
without being acted on from evidence that cannot support it.*

*One deviation from the text above: `certainty` uses the existing 10-minute
report window, not 10 s. A load that legitimately takes minutes and then faults
writes its report minutes after the stake, and a 10 s cutoff would file exactly
those genuine crashes as unattributed — discounting the slow loads, which is the
population the rule is trying to reason about.)* One of them may be
the operator losing patience with a slow load, and CLA-76 (m) takes 1.6 s
against its sibling's 509 ms.

---

## Already done, 3-4 August, so it is not re-litigated

- **Load progress** (`d3947f4`): the status names the plugin and warns the
  window will not repaint, published before the blocking call. This is what
  should prevent the next force quit at source. **Unverified on screen** — the
  first thing to check is whether the text actually appears before the pause.
- CLA-76 (m) is **released and not quarantined**; 4 ok / 1 crash on record.
- `release-quarantine` works, so nothing is lost while this waits — it just
  needs a manual release when it bites.

---

## Two corrections from the Drawmer 1973 crash (4 Aug 2026)

Both were found while diagnosing the campaign's first crash. Neither is optional:
each one, left as-is, makes the retry rule vouch for something it has not measured.

### 1. `prior_ok_in_ledger` MUST be stage-scoped

> **Measured 5 Aug 2026, and the diagnosis below is misattributed.** Drawmer's
> nine `ok` rows are on `/Library/Audio/Plug-Ins/VST3/Drawmer 1973.vst3` — the
> **sibling binary**, not the AU. The AU id carries exactly two rows, both load
> deaths. So Drawmer is an instance of correction **2**, and correction 2 alone
> already saves it; stage-scoping does nothing here.
>
> Stage-scoping is still load-bearing, on **4 plugins**, and the real instance is
> better than the stated one:
>
> | plugin | dies at | ok at | irrelevant successes an unscoped rule would credit |
> |---|---|---|---|
> | **SSL X-Gate** | `probe_gate_load` ×11 | `load` ×23 | 23 |
> | **bx_limiter True Peak** | `probe_gate_load` | `load` ×23 | 23 |
> | API-2500 (s) | `load` | `submit`, `sweep` | 23 |
> | CLA-76 (m) | `load` | `submit`, `sweep` | 25 |
>
> The top two are **M9's own signed subjects**. Unscoped, X-Gate reads as 23-time
> non-deterministic, gets released nightly, and re-crashes the probe gate every
> time — the exact loop the correction was written to prevent, reached by a
> different route than the one recorded.
>
> The rule is unchanged and correct. Only its stated instance was wrong.

Drawmer 1973 carries eight `"outcome": "ok"` rows in the ledger. Every one is
`"stage": "scan"` with `"detail": "1 description(s)"`.

**A scan describes a plugin without instantiating it.** Those eight say nothing
whatever about whether it loads — and in fact Drawmer 1973's AudioUnit has never
successfully loaded in ejmap, not once. Its only `load`-stage row is
`crash_on_load`.

An unscoped `prior_ok_in_ledger` would read "8 prior successes" and treat a
plugin that has never loaded as a well-behaved one having a bad roll — exactly
inverting the rule's purpose. It would then keep releasing it from quarantine
and re-crashing on it, nightly, forever.

**Rule: `prior_ok_in_ledger` counts prior successes AT THE SAME STAGE as the
failure being judged.** A load failure is vouched for only by prior load
successes. Same for `prior_ok_this_session`.

### 2. A crash attaches to a BINARY, not a product

The crash was `AudioUnit:Effects/aufx,0yow,SfTb`. The VST3 at
`/Library/Audio/Plug-Ins/VST3/Drawmer 1973.vst3` is a **different binary** by the
same vendor at the same version (Softube 2.5.62), and it has no crash history at
all.

Quarantine, retry counting, and the NON-DETERMINISTIC note must all key on
`plugin_id` (the format-qualified id already used in ledger rows), never on the
display name. Keying on "Drawmer 1973" would quarantine a working VST3 because
its AU sibling died, and would pool two independent determinism populations into
one meaningless rate.

Corollary for the operator, and worth surfacing in the UI eventually: when one
format crashes, **the other format is a legitimate next try**, not a retry of the
same thing.

### Method note

The absence of a quarantine row for Drawmer was read, during this diagnosis, as
evidence that `attribution: "unknown"` does not quarantine. It was not: the row
had been released by hand before the file was read. Nothing about the
attribution/quarantine relationship was established, and nothing here should be
built on that inference.

### 3. Drawmer 1973 re-quarantined after release (4 Aug 2026)

Released by hand, then quarantined again on a later launch. **Two crashes across two
separate process launches** — the first determinism evidence gathered under real
campaign conditions rather than from scan/load outcome ratios.

This is what N=3 is meant to consume. Note it does NOT yet make Drawmer's AU
deterministic-crashing: 2 of 2 with no successes is consistent both with "always
crashes" and with a high crash rate. `--load-once --repeat 3` (proposed below /
in session notes) is how that gets settled rather than argued.

---

## Subject added 4 Aug 2026: the Melda pair, UNRESOLVED not closed

`MLoudnessAnalyzer` and `MConvolutionMB` (both VST3, MeldaProduction 14.16) each
**crashed on load, 2 attempts of 2**, in separate processes with a fresh ledger root each
time. Quarantine rows written, `reason: crash_on_load`.

**This is recorded as unresolved, not as "always crashes".** 2-of-2 with no successes is
consistent with a deterministic failure AND with a high crash rate -- the same reading the
Drawmer note makes, and N=3 is not built. They are subjects for the retry rule, not
evidence for it.

**Why they matter beyond the crash:** these two share a VST3 uid (`80bb0df4`) at the same
version, so they share the identity key `format|uid|version`. Whether they also share a
FINGERPRINT depends on their parameter counts, and a parameter count needs a live instance
-- which is exactly what cannot be obtained while they crash. So the last open
wrong-product-serving risk is blocked behind a load failure, and `--load-once --repeat 3`
(or the retry rule itself) is what unblocks it.
