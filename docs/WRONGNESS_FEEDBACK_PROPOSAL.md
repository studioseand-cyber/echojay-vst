# Detecting a wrong dial — proposal

Proposed 5 Aug 2026. **Nothing here is built.**

A wrong semantic that dials *successfully* writes to the wrong knob and reports
nothing, because the write worked. At 1.3% of ~4,900 params that is ~64 silently
wrong dials. This is how they get found.

---

## Part 1 — "I trust the models more than my own answers"

### The comparison does not hold, and the truth is more useful than it

2 of 125 human rows wrong against 1 of 75 model agreements wrong is 1.6% against
1.3%. On those sample sizes that difference is noise, and the two numbers are not
measuring the same thing:

- **The 2 human errors were found by a check that can only see 47 rows.** The
  unit-family rule needs a semantic unit *and* a measured display unit. Within
  the population it can see, the human error rate is 2/47 = **4.3%** — and
  outside it, unmeasured.
- **The models' 1.3% is measured against the human answers.** It is a
  disagreement rate with a standard now known to contain errors, so it is not an
  independent error rate at all.

### And on those two rows the models were not better

| row | what the pipeline did |
|---|---|
| Transient Designer `Attack` | arms disagreed → **escalated** |
| HG-2 `Output` | Opus proposed `output_db`, **matching the wrong truth**; GPT refused → escalated |

The pipeline's virtue on both was **refusal, not knowledge**. It declined to
write. On HG-2 one arm agreed with the error.

### So the conclusion is better than "trust the models"

> **Neither the hand answer nor the model answer was authoritative. The
> measurement was.** In both cases the thing that turned out to be right was the
> unit the display declared, and the adjudicator was a mechanical rule over that
> measurement — not a judgement by either party.

That is a stronger position than picking a side, and it points the investment
somewhere different: **more mechanical checks over measured evidence**, not more
trust in whichever party is currently ahead.

### What it does change about recording corrections

One thing, and it follows directly from the premise. Today:

| gesture | source | trust |
|---|---|---|
| accept by reading | `human-confirmed` | `llm-classified` ✅ |
| **correct by reading** | `human-corrected` | **`human-verified`** ❌ |
| verify by touch | `human-verified` | `human-verified` ✅ |

Reading-to-accept correctly refuses to promote trust. **Reading-to-correct
promotes it — which is exactly the claim you have just withdrawn.** A hand answer
made by reading the same evidence the models read is not verification; it is a
third opinion.

**Proposed:** a correction made by reading records `human-corrected` and leaves
trust at `llm-classified`. Only `[t]` verify-by-touch produces `human-verified`.
Small change to `review.py`, and it makes the rule uniform: *touch is the only
thing that verifies.*

### Which of the 125 should be re-derived

Not all of them, and not the ones already adjudicated. The bounded set is **the
rows the unit-family check is blind to**:

| population | n | check status |
|---|---|---|
| semantic unit + display unit both present | 47 | **fully audited** — 2 found, nothing left |
| semantic declares a unit, display declared none | 69 | **blind** |
| semantic makes no unit claim (`drive`, `q`, `position`, `sensitivity`) | 9 | **no mechanical check possible** |

The 69 are where an error of exactly the same class hides undetected. Audit them
with the one independent source available: **the panel engraving**, via the
existing vision pass. Where the panel declares a unit the display did not, that
is a second unit-family check on evidence neither the human nor the text arms
had.

Cost: one capture per plugin (33), one vision call each. Not to re-open settled
rows — to find the ones the rule cannot see.

---

## Part 2 — The loop

Four tiers. **The first two need no user, no UI and no server**, and they should
be built first because they are the only ones that reach the long tail.

### Tier 0 — the unit-family rule at dial time (the biggest win, and it is nearly free)

`typedReadbackMatch` already parses the landed display in the semantic's unit
family, and `parseDisplayForUnit` already **extracts the display's unit token**.
It then throws it away unless it is a compatible conversion (`ms`↔`s`,
`hz`↔`kHz`). Everything else falls through to "take the number, return true".

That is why the live defect passed: asked `attack_ms −1.125`, display read
`"-1.13 dB"`, numbers agreed, **`match=true`**.

> **Proposed: a display whose declared unit contradicts the semantic's family is
> a mismatch, not a match.** `typedReadbackMatch` already returns three states
> (+1 match / 0 unverifiable / −1 mismatch) so there is somewhere for the verdict
> to go, and the revert-on-mismatch path already exists.

Constraints, both already established by the rule at map time:

- an **undeclared** display unit claims nothing → parse and compare as today
- a semantic with **no unit suffix** (`drive`, `sensitivity`) claims nothing

This is the same rule as `unitFamilyConflicts`, at a third call site, at the
moment of the dial. It would have caught the Attack case **in the field, on the
first dial, with nobody watching.**

### Tier 0b — the same rule over the whole corpus, server-side, continuously

It found 2 of 47 with zero user involvement. Every map ingested should be
checked, and every map already on the server re-checked once. No UI, no report,
no waiting for someone to notice.

### Tier 1 — the user report

**When.** Attached to the dial that just happened, while the change is on screen.
Not a general bug form — a report that cannot be attributed to a specific
`(fp, semantic, index, asked, landed)` is not actionable, and the chain already
displays what it dialled.

**What the plugin UI needs.** One affordance per dialled semantic on the row that
says what was changed: *"that changed the wrong thing"*. One click. Everything
else is already in hand — `ApplyResult` carries `semantic`, `index`, `normalized`,
`landedText`, `displayVerified`, `readbackMismatch`.

**Optionally, and worth far more:** *"which one did it move?"* If the user can
point at the control that actually changed, the report carries a **corrected
index**, which turns a complaint into a labelled correction.

### Can the server tell a plausible report from a mistaken one?

Partly, and from evidence the map already carries — which matters, because it
means corroboration does not depend on the report being trusted.

| evidence | what it says |
|---|---|
| **unit family** | the reported semantic contradicts the swept unit → the map was already wrong before the report |
| **stored readback** | `evidence.readback` holds asked/wrote/read per semantic; a display that never parsed cleanly in that family is latent evidence |
| **re-run the proposer** | the same offline classification, over the same control evidence — does two-model agreement side with the user or with the map? |
| **trust** | a report against `llm-classified` is a priori more plausible than one against `human-verified`. A prior, not a verdict |
| **corroboration** | two independent reports on one `fp+semantic` is much stronger than one, and it is the only signal that scales |

What the server **cannot** do is verify that the user heard what they say they
heard. So:

> **A single uncorroborated report never changes behaviour.** It queues for
> review and nothing else. Auto-correcting on one report makes every user's
> mistake everyone's outage, and makes the corpus writable by anyone.

**Evidence-gated quarantine**, which is the safe middle:

- **report + map-side corroboration** (unit family, readback, or the proposer now
  disagreeing) → **quarantine the semantic**: it stops being dialled — *fail
  closed, the same instinct as `mapIsDialableForSignals` withholding when the
  flag is absent — and queues for re-derivation.
- **report alone** → queue only, behaviour unchanged.
- **two independent reports** → quarantine, corroboration or not.

### Tier 2 — closing it back into the corpus

Re-derivation runs the proposer and the vision pass over that plugin. If the
result disagrees with the map, the row is corrected and the correction records
its provenance **including the report that triggered it** — the same shape as
`misclassified-<run>.jsonl`, which already exists locally as the review
correction sink. That file is the local half; this is what makes it round-trip.

### The honest limitation

**Report coverage is proportional to use.** A wrong dial on a plugin nobody dials
is never reported. The loop finds errors where they are exercised — which is
where they matter most — and leaves the tail unfound.

That is the argument for Tier 0 and 0b over everything else: **a static check
over measured evidence reaches all 4,900 params without anyone touching a
single one.** The report loop is what catches the residue that no rule can see,
and it should be built second, not first.
