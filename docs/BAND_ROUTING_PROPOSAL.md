# Band routing for the proposer — proposal

Proposed 5 Aug 2026. **Nothing here is built.** Scoping only.

Stage 2's first real run over 597 controls sent **78 rows to review as duplicate
semantics** — gate 4 refusing two controls that both wanted `freq_hz`, or five
that all wanted `gain_db`. Gate 4 is right (`params` is a dict keyed by semantic,
so the second write silently wins) but reviewing them one at a time is the wrong
answer when ejmap already has groups, the band matcher and manual entry.

## 1. What the 78 rows actually are

Reading every one of them across the 11 plugins that produced them. They are not
one thing, and two of the three classes must **never** become bands.

| class | rows | plugins | |
|---|---|---|---|
| **genuine multi-band** | **59 (76%)** | 8 | route to the matcher |
| **channel duplication** | 11 (14%) | 3 | NOT bands — one band, two channels |
| **genuinely distinct** | 8 (10%) | 4 | not a naming problem at all |

**Channel duplication** is the trap `EjmapBands.h` already warns about — *"holding
the rest literal is what keeps AMEK's TMT channel suffix from conflating channel
banks into phantom bands"*. In this corpus it appears as:

```
DPR-402 (s)               'Freq L/M'      vs 'Freq R/S'            (M/S pair)
Dangerous BAX EQ Master   'High Shelf Frequency 1' vs '... 2'      (instance, not band)
UAD Manley Massive Passive 'Ch1HiBW'      vs 'Ch2HiBW'             (channel prefix)
```

BAX Master is the one to be careful about: it has a **digit run**, which is the
strongest band signal there is — and here the digit is an instance number, not a
band. Its `Output Level 1` / `Output Level 2` give it away. A digit run alone
must not be sufficient.

**Genuinely distinct** is not a band problem and band routing cannot fix it:

```
UAD Vertigo VSM-3      'FETMix' / 'THDMix' / 'ZnBlMix'   three saturation stages
UAD 4K Channel Strip   'CMP Release' / 'EXP Release'     compressor vs expander
UAD Neve 88RS Legacy   'G/E REL' / 'L/C REL'             gate/exp vs lim/comp
AMEK EQ 200            'Mono Maker'                      a real freq control, not a band
```

These are different controls that each legitimately want the same semantic, and
`params` can hold one. That is a **schema** limit, not a naming one — see §6.

## 2. The corpus reading queue item 0 was waiting for

Item 1 settled API-550A's naming by reading it, and said explicitly: *"This is
ONE plugin. It settles API-550A and it does not settle the category... Do not
widen `prefixOrder()` on the strength of this."* Here is the corpus that decides
it — **five conventions across 11 plugins, and they are systematic rather than
arbitrary**:

| convention | example | in `prefixOrder()` today? |
|---|---|---|
| ordered prefix, whole token | `LF Freq`, `LMF Gain`, `HMF Q` | **yes** — AMEK, 4K, Neve |
| spelled-out ordered word | `Low Freq`, `Mid Gain`, `High Freq` | no — API-550A ×2 |
| compound, no separators | `Ch1LoFreq`, `Ch1LoMidFreq`, `Ch1HiFreq` | no — Manley |
| shelf-typed pair | `Low Shelf Frequency` / `High Shelf Frequency` | no — BAX Mix, SSL Fusion |
| digit run | `... Frequency 1` / `... 2` | yes (digit axis) — but see the BAX trap above |

So: **not forty conventions, and not one.** A synonym fold of the shape
`Low→LF, Lo→LF, Low Mid→LMF, LoMid→LMF, Mid→MF, High Mid→HMF, HiMid→HMF,
High→HF, Hi→HF` covers three of the four unhandled conventions and reaches
**7 of the 8 band-set plugins**. The compound case needs the fold applied to
camelCase-split tokens as well as whitespace-split ones.

**I am not proposing to widen `prefixOrder()` in this work.** It is a change to
the hand path's inference with its own gate and its own blast radius, and the
evidence above belongs in item 0's decision, not smuggled in through the
proposer. The proposer can carry its own fold and hand the matcher explicit
members, which is §4.

## 3. How the proposer recognises a band-shaped set

Runs after the per-row gates, before gate 4, over controls whose proposed
semantic is in `{freq_hz, gain_db, q, low_cut_freq_hz, high_cut_freq_hz}`.

**The rule is the one EjmapBands already uses: vary exactly one token, hold every
other token literal.** Tokenise on whitespace *and* camelCase boundaries, then:

1. Find token positions where the set differs and every other position is
   identical. **Exactly one varying position, or it is not a band set.**
2. Classify the varying token against, in order: the ordered prefix vocabulary
   (with the fold in §2), then a digit run.
3. **Refuse if the varying token is a channel marker** — `L/M`, `R/S`, `L`, `R`,
   `M`, `S`, `Ch<n>`, or a digit run that also varies on controls outside the
   band semantics (BAX Master's `Output Level 1/2` is what exposes it).
4. Require **≥2 candidate bands**, each with **≥2 members** drawn from
   `{freq_hz, gain_db, q}`.

Anything that fails these is not routed. It stays a gate-4 escalation and reaches
review exactly as it does today — the proposer's job here is to *reduce* the pile
safely, never to force a grouping.

## 4. What it hands the matcher

A **proposal**, not a group. The distinction matters: `EjmapBands.h` is explicit
that name patterns are hypotheses and *"nothing inferred enters a map
unverified"*. Every member here has been swept — they are in `map.controls` with
anchor tables — so the sweep requirement is met. The **grouping** is still a
hypothesis and must be confirmed, not asserted.

```json
"band_proposal": {
  "grouping_source": "model-proposed",
  "axis": "prefix",
  "varying_position": 0,
  "fold_applied": {"Lo": "LF", "LoMid": "LMF", "HiMid": "HMF", "Hi": "HF"},
  "bands": [
    { "ordinal": 1, "label": "LF",
      "order_evidence": { "from": "swept", "freq_index": 12,
                          "swept_hz": [15.0, 330.0], "sort_key": 15.0 },
      "members": { "freq_hz": {"index": 12, "name": "Ch1LoFreq"},
                   "gain_db": {"index": 13, "name": "Ch1LoGain"},
                   "q":       {"index": 14, "name": "Ch1LoBW"} } }
  ],
  "unassigned": [ {"index": 41, "name": "Mono Maker", "why": "no varying token in common"} ]
}
```

The proposer fills `index`, `name` and the members. It does **not** fill `trust`,
`method` or `captured_by` — those come from the sweep and the human, and it has
no standing to write them.

## 5. Ordering derives from swept frequencies, and only from those

This is the constraint the manual band entry work established and the thing most
easily undone by routing. Stated as a rule:

> **The name gives the grouping hypothesis. The sweep gives the order.** Two
> different questions, two different sources. Using the name for both is the
> failure mode.

So `ordinal` is assigned by sorting the candidate bands on the **measured**
frequency taken from each band's `freq_hz` member's anchor table — never by the
digit in the name, never by position in `prefixOrder()`, never by parameter
index. `order_evidence` records the derivation so it is auditable rather than
asserted, exactly as manual entry records `freq_source`.

Two cases where measurement cannot order a band:

- **No `freq_hz` member** (a gain+q band, or a fixed-frequency shelf). Manual
  entry already has the answer for the fixed case — `freq_source: "typed_fixed"`,
  confirmed with no index. The proposer cannot type a value, so a band with no
  swept frequency **gets no ordinal and the whole set escalates**. It does not
  fall back to name order.
- **Swept frequencies that do not separate** (two bands whose ranges are
  identical, which is what a channel pair looks like). This is a second,
  independent catch for §3's channel case: if the sort key cannot distinguish
  the bands, they are probably not bands. **Escalate rather than order them
  arbitrarily.**

## 6. When the matcher cannot group them either

Three outcomes, in order:

1. **Matcher confirms** → groups written with `grouping_source: "model-proposed"`
   and `trust: "llm-classified"` on the members. Stride verification stays where
   it is; the proposer feeds candidates and does not touch it.
2. **Matcher rejects** (hypothesis does not hold, stride unverified, members
   missing) → the set falls back to review as **one row per candidate band set**,
   not one per control: *"5 bands, LF/LMF/MF/HMF/HF, members freq+gain+q — accept
   the grouping?"* That is the throughput win, and it holds even when the answer
   is no.
3. **Neither** → manual band entry, which exists and which already sorts by swept
   frequency.

**The 8 "genuinely distinct" rows have no path here and should not be given a
fake one.** `FETMix`/`THDMix`/`ZnBlMix` are three real controls that each want
`mix_pct`. The honest options are (a) one becomes Tier 1 and the others stay
Tier 2, addressable by exact name — which `applySettings` already supports — or
(b) the schema grows a disambiguator. That is a separate decision and this
proposal does not make it; it just stops pretending band routing will solve it.

## 7. Sizing

If §3 and §5 hold, of the 78 duplicate-semantic rows:

| | rows | becomes |
|---|---|---|
| genuine multi-band | 59 | ~14 band-set questions (8 plugins) |
| channel duplication | 11 | stays in review, correctly refused |
| genuinely distinct | 8 | stays in review, correctly refused |

Review pile **304 → ~253**, and the 59 hardest rows become ~14 questions of a
kind that is much faster to answer. The non-band pile is unchanged at 225, which
is the session you are about to work.

## 8. Build order

1. The tokeniser and the varying-token rule (§3), with the channel refusals —
   testable offline against all 40 maps, no API calls.
2. Ordering from swept frequencies (§5), including both escalate cases.
3. The `band_proposal` document (§4).
4. The matcher handoff (§6) — C++, and the first part of this that touches ejmap.

1 and 2 are where the risk is and they are pure functions over data already on
disk, so they can be gated hard before anything is handed anywhere.
