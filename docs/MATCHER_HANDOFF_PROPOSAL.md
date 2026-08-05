# The matcher handoff — proposal

Proposed 5 Aug 2026. **Nothing here is built.** Scoping only.

`tools/propose/bands.py` groups 6 of 38 maps and refuses 32 with a stated reason.
Those 6 groupings have nowhere to go, so their rows still sit in the review pile
despite being solved. This is how they get to `EjmapBands` — and what happens
when it refuses them.

## The rule this is built around

> **The proposer supplies a grouping hypothesis. It never supplies an order, and
> it never supplies a verification.** Stride verification stays in `EjmapBands`
> untouched, and a proposal that fails it is refused rather than accepted on the
> proposer's say-so.

## 1. The problem the handoff has to solve, stated honestly

`BandInference::infer (capturedBand1, secondFreqIndex, paramNames)` takes **what
the human touched** and generates the rest from names. The stride is then
verified *against those captures* — the header is explicit that it is "derived
from the two indices the human actually captured" and is "a verifier, never a
generaliser".

The proposer has no captures. Every member it names is swept — they are in
`map.controls` with anchor tables — but nobody touched any of them, so **there is
no basis for the stride to verify against.**

That rules out the tempting shape. Deriving the stride from the proposer's own
members would be the proposer verifying itself, which is exactly the
generalisation the rule forbids.

**So the handoff does not remove the human from the loop. It removes the naming
work.** Today: capture band-1's members, capture a second frequency, then read
and correct a generated table. With the handoff: the table arrives already
filled, and the same two captures now *check* it instead of seeding it.

That is a smaller claim than "bands for free", and it is the one the evidence
supports.

## 2. What the proposer hands over

In the existing `proposals/<fp>.json`, beside `params` — same file, same
resume-by-presence, no new transport. `ProposalSet` gains a band section.

```json
"bands": {
  "grouping_source": "model-proposed",
  "axis": "prefix",
  "run": { "models": [...], "prompt_sha": "...", "effort": "high" },
  "bands": [
    { "label": "LF",
      "members": { "freq_hz": {"index": 12, "name": "Low Freq"},
                   "gain_db": {"index": 13, "name": "Low Gain"} },
      "order_evidence": { "from": "swept", "freq_index": 12,
                          "swept_hz": [34.6, 404.0], "sort_key": 118.2 } }
  ],
  "unassigned": [ {"index": 41, "name": "Mono Maker", "why": "..."} ]
}
```

**`ordinal` is deliberately absent.** `bands.py` computes one, and it is dropped
at this boundary. `EjmapBands` owns band order — its own comment says *"order IS
the claim: it decides band ordinals and what 'the highest band' means"* — and
manual entry already derives order from swept frequencies. Two owners of one
claim is how the claim drifts.

`order_evidence` **does** cross the boundary, and only as a cross-check: two
independent derivations of the same order from the same sweep should agree, and
disagreement is a signal that one of them is reading the data wrongly. It is
never an input to the ordinal.

Where the proposer could not order (`ordering: "unresolved"` — AMEK's LF/LMF both
sweeping 15..780 Hz), nothing is lost, because the ordinal was never going to
cross anyway. Only the evidence is thinner, and the record says so.

## 3. How `EjmapBands` receives a grouping it did not infer

A new entry point beside `infer`, **not a change to it**:

```cpp
/** Adopt a grouping proposed elsewhere. Produces the same BandInference an
    inference would, with every member marked unverified and the stride left
    for verifyStride to decide. It NEVER sets strideAgrees.
*/
static BandInference adopt (const ProposalSet::Bands& proposed,
                            const juce::StringArray& paramNames);
```

Three properties, each of which is the point:

- **It produces the same structure.** Everything downstream — the band card, the
  review strip, group assembly, the corpus gate — sees a `BandInference` and
  cannot tell how it was seeded. No second code path to keep in step.
- **`axis` comes from the proposal; `bands[].ordinal` does not.** `adopt` assigns
  ordinals by the same rule `infer` uses, from the swept frequencies, and then
  compares against the proposal's `order_evidence`. A mismatch sets `flag`.
- **`strideAgrees` starts false and `adopt` cannot set it.** Only
  `verifyStride`, against real captures, can.

Members arrive `captured = false`. That is what makes the two captures that
follow a *check* rather than a formality: the mapper touches band 1's frequency
and one other band's frequency, exactly as today, and the stride derived from
those two indices either agrees with the adopted table or does not.

## 4. What happens when the matcher refuses it

Three outcomes, and only the first writes a map.

| stride verdict | outcome |
|---|---|
| **agrees** | Groups written. `grouping_source: "model-proposed"`, members `trust: "llm-classified"` except the two the mapper touched, which are `human-verified`. |
| **unverified** (captured pair adjacent — amendment 3) | **Not accepted.** The existing rule already says a stride from positions 1 and 2 says nothing about position 7. The adopted table stays a hypothesis and the card asks for a non-adjacent capture. The proposer's agreement with itself changes nothing here. |
| **disagrees** | **Refused.** The table is discarded, the set falls back to today's `infer` from the captures, and the mapper carries on as if the proposal had never existed. |

A refusal is not a dead end for the mapper and it is **not a dead end for the
system either**: a grouping the stride refutes is precisely the labelled feedback
the proposal prompt needs, and it lands in `misclassified-<run>.jsonl` beside the
flat-param corrections — same file, same reasoning, already established as
*"MISMATCHES ARE A DATASET, not a burial"*.

**The corpus gate is unchanged and takes no exemption.** A model-proposed group
passes the same round-trip checks as a hand-built one or it does not ship.

## 5. Build order

1. **`ProposalSet` reads the `bands` section** — plumbing, no behaviour.
2. **`adopt`**, with its own gate: an adopted table is structurally identical to
   an inferred one, `strideAgrees` is false on arrival, ordinals come from the
   sweep, and an `order_evidence` mismatch raises `flag`. Testable with no
   plugin loaded, against the 6 real groupings.
3. **The band card offers the adopted table**, with the two captures now checking
   rather than seeding.
4. **The three verdicts** wired, including the `misclassified` append.

## 6. Before any of that — collapse band rows in stage 5

**This does not need the matcher and it addresses the pile today.**

`review.py` can group its escalations by the band sets `bands.py` already
recognises and present each as ONE row — *"5 bands, LF/LMF/MF/HMF/HF, members
freq+gain+q — is this grouping right?"* — instead of 15 separate freq/gain/q
rows. The answer is recorded as a decision on the set.

It does not write a map, so nothing about the rule above is weakened: the mapper
is confirming a hypothesis, and the grouping still has to reach `EjmapBands` and
survive the stride before it becomes a group. But it takes ~59 rows down to ~14
questions in the interface the mapper is using now, and it is a few hours of
Python rather than a C++ change.

**Recommended first**, ahead of item 1, precisely because the pile is the live
cost and this is the part of it that is already solved.
