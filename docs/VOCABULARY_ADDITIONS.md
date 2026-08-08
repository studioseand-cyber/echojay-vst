# Twelve additions to the dial vocabulary

Written 8 Aug 2026, from the 1,095-proposal corpus. **Approved and landed the
same day** — `VOCAB` in `evidence.py` and `DialSets` in `EjmapAssignment.h`, held
in step by an assertion in `test_propose.py` rather than by intent.

> ## THE NUMBER TO QUOTE IS ~500, NOT 1,235
>
> 1,235 controls carry these twelve words. **At most ~503 of them can become
> dialable**, because `params` is keyed by semantic and most of the twelve recur
> per band or per channel — a plugin with six crossover slopes records one
> `slope_db_oct`. The other ~730 stay reachable only by exact control name,
> which is what they are today.
>
> Anyone reading "1,235 controls covered" over-values this change by **2.5x**.
> The cap is queue item 17 and it is per-semantic: `density` and `tempo_bpm`
> lose nothing, `slope_db_oct` loses 83%. §3 has the table.

The 40-map pilot could not support this question and said so. The corpus can:
23,052 controls that both arms called `none` are not 23,052 failures, and the
useful question was never "how many" but "which of them are the same thing on
many different plugins".

---

## 1. What the `none` pile actually is

Splitting the 22,995 declined controls by what the sweep measured them to BE,
before reading a single name:

| | controls | |
|---|---|---|
| `mode` kind — enumerations and switches | 10,764 | 47% |
| `anchored` kind — continuous | 12,231 | 53% |
| …of those, **generic housekeeping slots** — `MIDI CC n`, `Param n`, `Modulators – Mod n`, preset/bank/program | 7,436 | 61% of the anchored set, from 43 plugins |
| **= real continuous controls with no word** | **4,795** | 9% of the corpus's 51,606 controls |

Four fifths of the apparent vocabulary gap is not a gap. A dial vocabulary is
*right* to have no word for `Bypass`, `Band 3 Type`, or `MIDI CC 47`: those are
switches, enumerations, and one vendor's generic parameter slots. Reading the raw
token table without this split makes `bypass` (356 plugins) look like the biggest
missing semantic in the catalogue.

**The twelve proposed here cover 1,235 of the 4,795 — 26% of the residue.** The
rest is a long tail of single-plugin tokens, which is exactly what a tail should
look like and is not addressable by adding words.

---

## 2. The twelve

Measured over the anchored, non-generic declines. `plugins` is the number that
carry at least one; **breadth is the test** — a token on many plugins is a
semantic, a token on one is that plugin's quirk.

| key | controls | plugins | unit family | units measured | what the consumer writes |
|---|---|---|---|---|---|
| `range_db` | 234 | **103** | `db` | none 129, db 105 | `{"range_db": -18}` — de-esser/gate/dynamics reduction depth |
| `depth_pct` | 113 | 51 | `pct` | none 74, pct 20, db 16 | `{"depth_pct": 40}` — modulation depth |
| `reverb_size` | 114 | 45 | *(none)* | none 69, db 38 | `{"reverb_size": 30}` — in the plugin's own scale, like `drive` |
| `width_pct` | 77 | 44 | `pct` | none 40, pct 36 | `{"width_pct": 120}` — stereo width |
| `mod_rate_hz` | 56 | 43 | `hz` | none 48, pct 6 | `{"mod_rate_hz": 0.5}` — LFO/AM rate |
| `tone` | 58 | 41 | *(none)* | none 45, pct 13 | `{"tone": 6}` — a tilt/character control, unitless like `drive` |
| `balance` | 58 | 34 | *(none)* | none 58 | `{"balance": -20}` — ER/tail, front/rear, wet/dry position |
| `slope_db_oct` | 192 | 33 | *(none)* | db 105, none 66 | `{"slope_db_oct": 24}` — filter/crossover slope |
| `hold_ms` | 101 | 31 | `ms` | ms 78, none 19 | `{"hold_ms": 50}` — gate/peak hold |
| `density` | 29 | 29 | *(none)* | none 29 | `{"density": 50}` — reverb density |
| `tempo_bpm` | 28 | 28 | *(none)* | none 28 | `{"tempo_bpm": 120}` |
| `pan` | 80 | 21 | *(none)* | none 78 | `{"pan": -30}` |

**On the unit families.** The suffix is not decoration — `unitFamilyConflicts` is
the mechanical backstop that found 2 of 2 errors in the hold-out that four models
missed, and a semantic's suffix is the claim it gets checked against. Three rules
were applied:

- **Claim the family only where the measurement supports it.** `hold_ms` (78 of
  101 measured `ms`) and `range_db` (105 measured `db`, the rest undeclared)
  claim; a display that declared no unit claims nothing on either side, so the
  undeclared majority costs nothing and the check still fires on a `hold` that
  turns out to be a percentage.
- **Do not invent a family the C++ does not have.** `semanticUnit` knows
  `db / ms / hz / pct / s / ratio`. dB-per-octave and BPM are not in it, so
  `slope_db_oct` and `tempo_bpm` are named for the reader and make **no**
  machine claim, the same way `drive` and `sensitivity` do today. Adding two
  families to satisfy a naming instinct would change a rule the hold-out was
  measured against.
- **Scope the ambiguous ones.** `reverb_size` rather than `size`, `mod_rate_hz`
  rather than `rate` — the bare words collide with `Out Echoes Size` and
  `Sample Rate`, and a semantic that means two things cannot be adjudicated by
  any mechanical rule.

### Held back, deliberately

**`speed` — 109 controls on 34 plugins, not proposed.** It passes the breadth
test and fails the *one meaning* test: `VariSpeed` on a tape emulation is a
percentage of nominal, `Dynamic detection – Auto speed` on MDynamics is a time
constant measured in ms (34 of the 109 declared `ms`). That is two semantics
wearing one word, and shipping it would put a rule in the corpus that no unit
check can save. It needs splitting before it needs adding.

---

## 3. What item 17 caps

Item 17 — *one semantic per `params` key* — is not a blocker for any of the
twelve, because option (a) works today: promote one control to Tier 1 and leave
the rest name-addressable, which `applySettings` has resolved by exact control
name since 31 July. But it **caps the yield**, and the cap is very uneven:

| key | plugins with MORE THAN ONE | what that means |
|---|---|---|
| `slope_db_oct` | **91%** | every multiband unit has a crossover slope per band |
| `pan` | **86%** | `Source Pan` / `ADT Pan`, `Output_Pan_A` / `_B` |
| `reverb_size` | **76%** | `Reverb Size` and `Out Echoes Size` on one plugin |
| `hold_ms` | 58% | per band on the Melda multibands |
| `balance` | 50% | `ER/Tail` and `Front/Rear` on one reverb |
| `range_db` | 44% | per band, and gate-vs-de-esser on one strip |
| `width_pct` | 41% | |
| `depth_pct` | 37% | |
| `tone` | 27% | |
| `mod_rate_hz` | 23% | |
| **`density`** | **0%** | one per plugin, every time |
| **`tempo_bpm`** | **0%** | one per plugin, every time |

**The arithmetic that matters:** 1,235 candidate controls, but at most **one
Tier 1 row per plugin per semantic** — so **~503 become dialable by a generic
request** and the remaining ~730 stay reachable only by exact name. That is not
a loss relative to today (today they are `none`, reachable by name, and nothing
changes for them), but it does mean the twelve buy roughly 500 generic dials,
not 1,235.

`density` and `tempo_bpm` are the only two that are free of item 17 entirely, and
`slope_db_oct` is the one where item 17 eats 83% of the count — worth knowing
before anyone reads its 192 as the prize.

---

## 4. What it costs to apply to the existing corpus

**A re-derivation cannot do this.** `regate.py` re-decides from the stored arms
without paying again, which works for a changed *gate* — it is how the hedged
agreements were recovered for nothing. It cannot work here: the arms never had
these twelve words in their prompt, so there is no stored answer to re-read. The
words have to be asked.

Four options, at the planning rate of **$3.29 per 1,000 controls** (from the
$142 full-run projection over 43,194):

| | population | controls | cost | what it can change |
|---|---|---|---|---|
| **A — re-ask every decline, corpus-wide** ✅ | all 1,095 maps | 22,995 | **~$76** | any `none` → a new semantic. Cannot revisit an accept or an escalation. **No name filter.** |
| B — re-ask declines in the 327 maps that already show a candidate token | 327 maps | 13,179 | ~$43 | same, minus whatever a name filter misses |
| C — full `--force` over those 327 maps | 327 maps | 23,979 | ~$79 | also revisits their escalations and accepts |
| D — full `--force`, whole corpus | 1,095 maps | 43,194 | ~$142 | everything |

**A was chosen and ran on 8 Aug.** B was cheaper and B was the trap: selecting
maps by whether a control's *name* already contains one of the twelve tokens is a
filter whose discards nobody measures.

### The argument is now a measurement

Of the **542 new semantics the re-ask accepted, 140 — 26% — sit on controls
whose name contains none of the twelve words.** Option B would have discarded
every one of them, and nothing in its output would have said so:

```
C1 comp-gate (m)      'Floor'        -> range_db
elysia karacter mix   'Color'        -> tone
SSL Native X-Sat v6   'Shape'        -> tone
UAD RealVerb-Pro      'Diffusion'    -> density
VerbSuite Classics    'Chorus Level' -> depth_pct
SSL X-Delay           'Modulation'   -> depth_pct
SSL X-Delay           'Diffusion'    -> density
SSL X-Delay           'Internal BPM' -> tempo_bpm
PRS V9 (m)            'Speed'        -> mod_rate_hz
Unfiltered Audio ID   'Jitter'       -> depth_pct
UAD Tonelux Tilt      '12dB per Oct' -> slope_db_oct
```

`Spread -> width_pct` was the hypothetical used to argue against B. `Floor ->
range_db` and `Diffusion -> density` are the instances. The saving would have
been $33; the cost would have been a quarter of the yield, invisibly.

This is the second time a filter's discards turned out to be the whole question
here — the first was the confidence gate, which manufactured 34% of the review
pile. Score the discards, not the accepts.

A needs a small mode in `propose.py` that does not exist: **re-ask only the
declined controls of a map, and merge the answers against the accepted params
already in the proposal**, so gate 4 still refuses a new `range_db` on a plugin
whose accepted set already holds one. Same invariant as chunking, same reason.

---

## 5. Order of work

1. ~~Add the twelve to `VOCAB` (`evidence.py`) **and** to the C++ `DialSets`
   (`EjmapAssignment.h`)~~ — **done 8 Aug.** A word the mapper cannot pick in the
   app is not in the vocabulary, it is only in the proposer. Category placement
   is measured, not guessed: a semantic joins a category when ≥5 of its plugins
   carry one, or ≥⅓ of the category's maps do (which is what puts `range_db` and
   `hold_ms` on `gate`, only 8 maps but both canonical there).
2. ~~`test_propose.py`: assert the two lists agree~~ — **done.** Read out of the
   header and compared, the way `semanticUnit` already is. A rule that exists
   twice is two rules. `nameSuggestsDialSet` also gained the twelve tokens: it
   is a *withholding* filter, so a dialable word missing from it would let the
   I-bulk floor skip the very control the vocabulary can now name.
3. ~~Re-ask-declines mode with gate 4 merged against existing accepts~~ —
   **done**, `propose.py --redo-declines`. Three invariants tested: only the
   declines are asked; a new claimant cannot take a semantic the proposal
   already holds (and the incumbent is *not* withdrawn — it was decided by a run
   this one is not re-opening); a failed re-ask leaves the file byte-identical.
4. Run A (~$76). ← **next**
5. Only then decide item 17 — the yield table above is the input to that
   decision, and it did not exist before this corpus.

---

## 6. A number that is now unknown, and worth measuring

The eleven giants were proposed in **chunks**, so their arms judged each control
with only ~200 siblings in view instead of the whole surface. The 98.7%
auto-accept precision was measured on whole-plugin prompts. **It does not cover a
chunked row, and there are 8,407 of them** — 16% of the corpus, concentrated in
exactly the plugins with the most controls.

This is a known unknown, not a suspected defect: nothing says the number is
worse, only that it was not measured under these conditions. The measurement is
cheap and already has a harness — `propose.py --audit` re-derives semantics a
human already confirmed and scores against them. Running it over a chunked
population with `--chunk 200` against the same hold-out would give the chunked
figure directly. It needs hold-out rows on a giant, which the 266 may not
contain; if not, the cheaper version is to re-propose a handful of *medium*
plugins both ways and compare the two answers to each other.

`run.chunking` marks every affected proposal, so the population is queryable
whenever someone wants to pay for the answer.
