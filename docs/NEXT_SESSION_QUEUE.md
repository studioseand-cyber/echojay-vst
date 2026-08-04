# Queued for next session

Three items, in the user's order. Nothing here is started.

---

## 1. Persist the band diagnostic

**The defect, measured 4 Aug 2026 on API-550A (`aufx,A5AM,ksWV`):**

The band inference reported "0 bands inferred from your 3 touches" to the screen and
persisted NOTHING. `assign-bdc578da…json` holds exactly:

```
fp · plugin_id · category · rows · ignore_rows · pending_controls
lockstep_observed · accepted_groups · controls_excluded
```

No `bandPlan`, no `strideNote`, no captures. The three touched indices and their
parameter names died with the prompt. `accepted_groups: 0` is the only trace and it is
**indistinguishable from never having attempted the inference at all.**

`strideNote` is built for exactly this — `EjmapBands.h` sets it to
`"no digit or band-prefix token in '<name>'"`, naming the parameter it failed on. It
reaches the screen and goes no further.

**What to record in the session file when an inference runs:**

- `strideNote` verbatim
- the touched parameter indices
- their parameter names as the plugin reports them
- `axis` (`"digit"` | `"prefix"`) and whether stride verification ran
- enough to distinguish *attempted and found nothing* from *never attempted*

**Then:** the user re-runs API-550A's bands and we READ the note instead of recalling
the product. Do not diagnose the naming question before that read exists — see below.

### What is already established (do not re-derive)

`EjmapBands.h:100`:

```cpp
static juce::StringArray prefixOrder()
{ return { "LF", "LMF", "MF", "HMF", "HF" }; }
```

- There are TWO axes, `"digit"` and `"prefix"`. **Numbering is not required** — LF/MF/HF
  EQs are explicitly supported and the order is treated as the claim.
- The match is `prefixOrder().contains(toks[i], true)` — a WHOLE-TOKEN comparison.
  `"LF Freq"` → `["LF","Freq"]` matches. `"Low Frequency"` → `["Low","Frequency"]` does
  not: "Low", "Mid", "High", "Low Mid", "High Mid" are absent from the vocabulary.

### What is NOT established

**API-550A's actual parameter names.** The hypothesis that it spells them out is
RECOLLECTION of the product, not a reading of its parameter list, and it must not be
treated as a finding. The scope question — one plugin, or every vintage-console EQ named
by frequency range — is open until real names are read from two or three EQs.

If the note confirms spelled-out names, the likely fix is widening `prefixOrder()` plus a
synonym fold (Low→LF, Low Mid→LMF, …). The two-axis machinery already does the hard part;
nothing structural is implicated.

---

## 2. The parked state

**Parking already works. Nothing says so, which is why it has been avoided.**

Established: `persistSession()` writes on every accept; `restoreSession()` matches on `fp`
and works across restarts (it is the mechanism that restored CLA-76's rows). Scanning or
loading another plugin does not touch `assign-<fp>.json`. API-550A's file holds real state
— `output_db` confirmed at index 12, two `not_present`, bands and controls deferred.

NOT established: a full park-and-return round trip has never been watched. **The user is
testing this manually first and will report.** Read that report before building.

**Propose (all of it reads disk and asserts nothing):**

- a **parked** state in the list — a plugin with a session file and unresolved rows is
  neither unmapped nor mapped, and column 1 currently calls it unmapped
- a line on leaving that says the work is saved
- a line on return saying what was restored and where the mapper left off (row counts are
  already in the file)

The capability needs nothing. The gap is entirely in what is said and shown, and silence
about an honoured guarantee is what makes a mapper rescan and re-map instead of parking.

**Why it matters at volume:** the intended workflow is park a plugin that is fighting you,
do three easy ones, come back.

---

## 3. Still queued from earlier sessions

- **Row-index keys in `map-state.json`** — keys `"0"`, `"3"`, `"7"` sit beside 1,799 proper
  `format|uid|version` strings. Row indices shift between scans, so those entries will point
  at whatever occupies that row next time: a column reading confidently WRONG rather than
  unknown. Find the writer passing an index where an identity belongs; make the loader
  reject any key not matching the identity shape rather than trusting the file.
- **The retry rule + force-quit attribution** — `docs/NEXT_SESSION_RETRY_AND_ATTRIBUTION.md`,
  including the two corrections appended 4 Aug (stage-scoped `prior_ok_in_ledger`; crashes
  key on the binary, not the product) and Drawmer's re-quarantine after release.
- **The refusal audit** — `docs/NEXT_SESSION_REFUSAL_AUDIT.md`. Read every user-facing
  refusal and ask whether that code path measured what the message claims. The keyword
  sweep already run is NOT that audit.

## Related tooling proposed but not built

`--load-once <plugin_id> [--repeat N] [--editor] [--render]` — a single-plugin headless
load. Wanted twice in two days: Drawmer 1973 could not be reproduced because `--probe-batch`
needs a map, and the plugins worth diagnosing are precisely those that never got far enough
to have one. Separate child process per attempt, one JSON line per attempt.
