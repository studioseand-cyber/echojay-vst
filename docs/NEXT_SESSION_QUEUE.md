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

---

## 4. Manual band entry — ACCEPTED AS PROPOSED, not yet built

The typed-anchors equivalent for bands: when inference cannot group a band, the mapper
says which indices form it. **Build item 1 BEFORE this** — see the scope note at the end,
which is a design reason and not merely a size one.

### What a band group needs

- **Required: `freq_hz` + `gain_db`.** A band the consumer cannot place or cannot move is
  not dialable. This is also the pair the band matcher already routes as a unit on
  grouped EQs.
- **`q` — optional, and genuinely often absent.** API-550A has none; several vintage EQs
  fix or step it. A group without `q` dials fine.
- **`enable` — optional to HAVE, mandatory to HONOUR.** If a band has an enable reading
  off, the freq/gain writes are inert and the dial silently does nothing. A manually
  entered band goes through `enableLinkFor` / `checkEnableIsNull` exactly as an inferred
  one does.

### Interaction: synthesise rows, do NOT build a picker

Do not ask "which of these three is band 1's frequency?" — that is a new widget and a new
mental model. Instead: one count prompt ("how many bands?"), then synthesised assignment
rows, one per band-slot:

```
band 1 frequency      unresolved
band 1 gain           unresolved
band 2 frequency      unresolved
```

Each resolves through the flow the mapper already knows — touch, W to sweep, confirm.
`q` and `enable` rows are offered and D-able without penalty, since both are legitimately
absent. The panel already renders and resolves rows generically, so mismatch warnings,
readback and skip reasons all come along free.

### Recording: TWO CLAIMS, RECORDED SEPARATELY

```json
{ "n": 1, "label": "LF", "grouping_source": "mapper",
  "ordering_source": "measured",
  "params": { "freq_hz": {...}, "gain_db": {...} } }
```

- `grouping_source: "mapper" | "inferred"` — who decided these indices form a band. Human
  is stronger evidence FOR THE GROUPING.
- **Per-parameter evidence is unchanged** — sweep anchors, unit family, trust, readback.

The mapper's say-so groups them; the sweep says what they do. Nothing asserted is ever
promoted to a measurement. Do not merge these into one trust field.

### Ordering: the mapper supplies the LABEL, the tool derives the ORDER

The mapper supplies `LF`/`MF`/`HF` (or `1/2/3`). Ordering is derived from the **swept
frequency range** — each freq slot is swept anyway, yielding a real lo/hi — and bands sort
by measured midpoint. Then:

- **agree** → `n` assigned from measured order, both recorded, the label rides along
- **disagree** → **REFUSE and show both.** "You labelled index 4 as LF, but it sweeps
  800–8000 Hz and sits above the band you called MF." Either a mislabel or a mis-picked
  index; both are worth stopping for.

> **DO NOT "SIMPLIFY" THIS BACK TO ENTRY ORDER.** This is the strongest part of the design
> and it is counterintuitive, so someone will try.
>
> Deriving order from measured frequency makes the manual path **better than inference on
> the ordering claim**, not a weaker fallback. Inference reads order from NAME POSITION
> (`prefixOrder()`'s LF→LMF→MF→HMF→HF); this reads it from BEHAVIOUR. M5 treats
> LF-below-MF-below-HF as *the claim*, so the manual path must ESTABLISH that claim rather
> than inherit it from the order the mapper happened to type.

### Two cheap checks (the sweep already runs; these cost nothing)

- **Unit sanity** — a slot claimed as frequency that sweeps to a dB unit family is
  refused, and vice versa. Catches the commonest slip: picking the gain when you meant
  the freq.
- **Distinctness** — two bands with identical swept ranges suggest the same parameter
  picked twice, or a channel bank rather than distinct bands. Flag; refusal optional.

### SCOPE NOTE — why item 1 comes first

Item 1 (persisting `strideNote`) decides whether manual entry is an **escape hatch** or
the **main road**, and that changes what should be built.

If most vintage EQs miss because their names are spelled out ("Low Frequency" rather than
"LF"), then widening `prefixOrder()` plus a synonym fold (Low→LF, Low Mid→LMF, …) is the
cheaper fix and manual entry stays the exception. Building manual entry first would be
building an exception path for the common case.

**So: item 1 first for this reason, not merely because it is smaller.**
