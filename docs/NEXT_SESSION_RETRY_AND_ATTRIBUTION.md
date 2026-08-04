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

### The proof, required before it lands

1. Force a load failure and confirm **three separate launches** before quarantine.
2. Confirm a plugin **succeeding on attempt two is never quarantined**.

A quarantine change that ships unproven is worse than today's rule, which is
wrong but predictable: a half-tested retry could fail to quarantine something
that genuinely crashes every time.

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

**An unattributed death does not count toward the three.** One of them may be
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
