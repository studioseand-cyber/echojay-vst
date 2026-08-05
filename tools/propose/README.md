# tools/propose — stages 2 and 5 of the semantic pipeline

Offline. **Never loads a plugin.** Reads `~/Library/ejmap/maps/*.json`, proposes
Tier 1 semantics for controls no human answer already claims, and hands what it
cannot settle to a review interface.

Design and scoping: `docs/PIPELINE_PROPOSAL.md`.

## Setup

```sh
python3 -m venv .venv && .venv/bin/pip install anthropic openai
export ANTHROPIC_API_KEY=... OPENAI_API_KEY=...
```

## Stage 2 — propose

```sh
python3 propose.py                       # every unproposed map
python3 propose.py --only CLA-76         # substring match on the plugin name
python3 propose.py --effort medium       # see "effort" below
python3 propose.py --audit               # score against already-confirmed params
python3 propose.py --force               # redo maps already proposed
```

Writes `~/Library/ejmap/proposals/<fp>.json` in the shape `ProposalSet::load`
already reads (`{category, params:[{index, kind, confidence, reason}]}`), with
provenance added alongside. Unknown fields are ignored by the C++ side, so the
extra record costs no schema change.

**Resume is the file layout**: one file per fingerprint, presence means done.
Nothing else. A crash costs the plugin in flight.

## Stage 5 — review

```sh
python3 review.py --summary              # the shape of the pile, decide nothing
python3 review.py                        # work it; [q] saves and quits anywhere
```

Writes `~/Library/ejmap/decisions/<fp>.json` as each row is decided, and appends
corrections to `misclassified-<run>.jsonl`.

> Feeding decisions back into a map is **not built** — that is the C++ submit
> path, and the next step after this tool.

## Four things that are load-bearing

**1. Both arms see byte-identical text.** The whole design rests on two models
agreeing; two models given different prompts agreeing means nothing. The prompt
is hashed into every proposal file (`run.prompt_sha`) so a row written today can
be reconstructed against the text that produced it.

**2. Every gate can only ever REFUSE.** Four of them, in `merge()`: both arms
named the same semantic, both were confident, the unit family does not
contradict the measured unit, and no other control on the plugin claims that
semantic. Nothing in this tool can promote a row on its own.

**3. Reading is not touching.** Accepting a proposal at review records that a
human *looked* — `semantic_source: human-confirmed`, and trust stays
`llm-classified`. Only a correction or a verify-by-touch produces
`human-verified`. A list of confident claims invites blind acceptance, so
acceptance is not allowed to launder trust.

**4. A failure writes nothing.** Because presence means done, an arm that fails
must not leave a file — otherwise a transient 529 is recorded permanently as
"this plugin was 100% unresolvable". That happened, to 11 of 33 plugins, before
it was fixed.

## effort

The hold-out measured 98.7% auto-accept precision at **effort high**, so that is
the default and anything else is a different configuration. It is a flag rather
than a constant because `high` is periodically 529-Overloaded for capacity
reasons, and the value is **recorded on every row it produced** — in `run.effort`
and in each param's `reason` — so a proposal can never be mistaken for one made
at a different setting.

Measured on the same 125 confirmed params (`--audit`):

| effort | auto-accepted | of those, matching the human |
|---|---|---|
| high | 75/125 = 60.0% | 74/75 = 98.7% |
| medium | 70/125 = 56.0% | 70/70 = 100% |

One run each; 70/70 does not establish a better error rate than 74/75.

## Known limitation: flat params only

The proposer has no concept of band groups. On a multi-band EQ it proposes
`freq_hz`/`gain_db`/`q` once per band, gate 4 refuses them all as duplicate
semantics, and every one lands in review — 79 of 304 review rows on the first
real run. Those controls belong in the band matcher, which ejmap already has.
Routing them there is the next build.

## Gate

```sh
python3 test_propose.py
```

Includes reading `semanticUnit` out of `Source/EchoJayParamApply.h` and asserting
the Python mirror matches it rule for rule — the copy is asserted, not trusted.
