# Chain-intent classifier: Implementation Plan

**Date:** 1 Aug 2026
**Companion docs:** `CHAIN_AI_BUILD_SPEC.md` (plan-of-record entry + the two register entries this plan answers), `scripts/test-chain-intent.mjs` on the saas side (the fixture seed)
**Status:** agreed plan; builds AFTER the 2.25.0 release. The downgrade counter ships with 2.25.0, so the shadow baseline accumulates for free from release day.
**Target:** echojay-saas (`/api/classify`, chat.js routing), echojay-vst-v200 (split call, provisional bubble, templated ack)

---

## 0. Why this exists (the incident, compressed)

One evening, 1 Aug 2026: "Add the AMEK EQ 200 and cut 300 Hz by 2 dB, boost
8 kHz by 1.5 dB" misrouted to chat three times in three shapes (racked,
empty rack, fresh chat). The message matched NONE of four regex arms:
CHAIN_REQUEST_RE requires the literal word "chain", EDIT_REQUEST_RE's
add-arm requires a positional. The client had done everything right (feed
attached, 740 names, staged chain_generate); the server downgraded to chat;
the chat note then FORBADE the block; the model claimed "Done" for work
that never happened, twice, and the false claim entered history where the
next turn treated it as real.

The economics that motivate a model call: a misroute is not a failed turn,
it is a WRONG EXPENSIVE turn. The chat note forbids a block, so the model
either offers (a wasted round) or lies (an honesty violation that
self-reinforces through history). A classification miss costs a full chain
turn plus the violation plus usually a corrective user turn. A few hundred
input tokens and a one-word answer is cheap against that.

Every miss presents as the MODEL being unhelpful, never as routing. That is
why four arms could be wrong about the plainest chain request possible and
nobody saw it until a live test.

---

## 1. Decisions taken

| Question | Decision | Reasoning |
|---|---|---|
| Where it sits | **Option B: model on every turn classifyChainIntent handles today. Regexes retired from routing, retained ONLY as the stated failure fallback.** | The disagreement-only option (regex fast path, model on disagreement) collapses on a measured fact: the client stages chain_generate on effectively every feed-carrying send (the 14 Jul billing note documents this), so "client staged chain, regex says chat" fires on nearly every plugin turn INCLUDING the plain questions the regex downgrades correctly. A's model call runs almost as often as B's while keeping two routing regimes and giving the fast acknowledgement to only some turns. The downgrade counter cannot make A cheap because its trigger is unselective. |
| Output shape | **One constrained enum: `chat \| chain_generate \| chain_edit \| ambiguous`. No prose. Structured, intent field only.** | The one thing that must not degrade is the classification. Model-written acknowledgement prose buys warmth at the cost of exactly that, so the ack moved client-side entirely (below). Enum-only output also collapses cost and latency: ~5-10 output tokens. The fixture eval then measures classification and nothing else. |
| Acknowledgement | **Client-templated from the returned intent plus names the client resolves itself (its resolveByName machinery already exists). Not a degradation fallback: the design.** | Zero output tokens at risk, no completion-tense copy possible by construction, nothing to strip from history because the client owns the string. Templates: chain_generate "Putting that together - <names>...", chain_edit "Working on your rack...", chat/ambiguous no bubble (existing shimmer only). Copy stays forward-looking, placement-register, per ACTION HONESTY. |
| Transport | **Split call. Client fires `/api/classify` first, renders the ack at ~1 s, then fires the main call carrying the resolved intent (trust-but-validate, same contract as chip intents).** | The plugin's chat transport is a one-shot POST with no streaming, so the ack cannot ride the main response. The split call turns classifier latency into a time-to-first-content IMPROVEMENT: today nothing appears until the full chain reply lands (5-20 s); with the split call the first visible content arrives at ~1 s. The classifier call reduces perceived latency rather than adding to it. |
| Failure semantics | **Stated, not inherited: timeout (~2.5-3 s budget) or unparseable output falls back to the CLIENT'S STAGED LABEL, then regex (web clients have no staging), NEVER bare chat.** | The inversion today's incident earned. The asymmetry: over-classifying to chain produces a DECLINABLE PROPOSAL (preview-then-confirm makes it free; the produced-type re-weighting already refunds chain-classified turns that emit no block). Under-classifying to chat produces the forbidden-block state where the model offers or lies. Defaulting to chat is what hid the bug for however long it existed. |
| Model disagrees with client staging | **Model wins. Counted BOTH directions (`intentOverride` with direction, same persistence + fixture-test discipline as `intentDowngrade`).** | Client staging is a PRIOR, not truth: it was right on 1 Aug and wrong for months before 14 Jul (staging chain on every feed turn, billing plain questions as premium). The classifier exists to be better than both; the counter proves whether it is. |
| Ambiguity | **`ambiguous` is a real third label. Routes to the chat prompt shape PLUS one sentence: the request may be actionable; act (CHAIN_EDIT with a rack, an offer without one) or ask via chips - plain advice alone is not a complete answer to a request that may be an instruction.** | "Make the vocal brighter" with a rack is genuinely advice-or-edit. Today those turns silently become chat, and the failure is not that chat cannot act (chat+[CURRENT CHAIN] carries the edit permit, proven live 17:59 1 Aug) - it is that nothing tells the model advice alone is an incomplete answer. Bills as chat (daily pool, user-favourable); the produced-type UPGRADE pass re-bills if a block is produced and the refund pass covers an ask - zero new billing machinery. Persisted as its own value, never folded into chat: the ambiguous RATE is a health metric, and a classifier that shrugs is not a classifier (gate below). |
| Model choice | **Decided by the fixture eval, not by price. If Haiku 4.5 ties Sonnet-class on the fixture, take Haiku for the LATENCY, not the cost. If it does not tie, Sonnet and done.** | See pricing (section 5): the delta between models per turn is under $0.002 while a misroute costs 20-100x either model's call. Cost cannot be the deciding axis; classification accuracy first, then TTFT. |
| Sequencing | **After the 2.25.0 release.** | The release already carries more than planned (set op, state-preserving replace, band reach, exposure on edit paths, the fifth regex, both counters, scan hold, card fix). The counter's first week in production is the shadow baseline for free. |

---

## 2. What the classifier sees

- The user's TYPED portion (userTypedPortion strip, not the raw content -
  matching against raw content matches the feed, proven with the exposure
  scoping work).
- The client's staged label, framed as a prior.
- Whether `[CURRENT CHAIN]` is present (a rack exists).
- Whether the plugin feed attached.
- The tail of the prior assistant reply (the offer-acceptance arm needs it:
  "yes please" after "want me to put that together as a chain?").

All cheap tokens, all real signal. The staging fact matters because on 1 Aug
the client was right and the server was wrong; the pre-14-Jul history
matters because the reverse held for months. Both go in; neither is truth.

---

## 3. Fixture and shadow phase

**Fixture** (grows from the reply store, which holds every real turn, and
from the `[chain-intent-downgrade]` log's typed excerpts, which are the
ongoing feed of missed shapes):

- **Miss set** - the 1 Aug misrouted turns, ground-truth labelled:
  - 17:59:36 racked "Add the AMEK EQ 200 and set the mono maker..." (truth: chain_edit)
  - 18:58:05 "this channel" answer turn after the add request (truth: chain_generate via the clarify round)
  - 19:00:30 retry, poisoned history, "same moves as before" (truth: chain_generate)
  - 19:55:00 fresh chat, empty rack, direct request; the noblock tripwire's first live catch, honesty=['Done'] (truth: chain_generate)
- **Regression set** - turns the regexes get RIGHT, seeded from
  test-chain-intent.mjs's ten cases: offer-then-yes pairs, plain questions,
  the past-tense mention ("I added more compression yesterday"), capture
  pass-throughs, edit verbs with and without a rack.
- **Ambiguous cases** with the label as ground truth ("make the vocal
  brighter" + rack), and explicitly NON-ambiguous neighbours ("Add the
  AMEK..." is chain_generate, never ambiguous) so the label cannot become
  the model's escape hatch.

**Shadow phase**: the model classifies and logs on every eligible turn;
the regex still routes. No ack UX during shadow (the ack requires the model
to actually route). Runs on production traffic post-2.25.0.

---

## 4. The cutover threshold (written 1 Aug 2026, BEFORE any shadow data exists)

Fixture gates (hard, pre-shadow):
- 100% on the miss set.
- On the regression set, every model-vs-regex disagreement is read
  INDIVIDUALLY, never counted: a disagreement where the model is right
  updates the fixture label; one where the model is wrong blocks until
  fixed and re-evaled.

Shadow gates (live):
- Minimum 300 classified turns or 14 days, whichever comes first.
- Every live disagreement reviewed individually: model-right confirms;
  model-wrong files a fixture case, and cutover blocks until that case
  passes.
- A model-wrong that would have produced a wrong BLOCK (not merely wrong
  billing) blocks cutover unconditionally until fixed.
- Ambiguous rate above 10% of chain-eligible turns means prompt tuning
  before cutover.

On cutover: regexes demoted to the no-staging fallback (web) and the
timeout fallback order becomes client staging, then regex, never bare chat.
`intentOverride` (both directions) replaces `intentDowngrade` as the
primary counter.

---

## 5. Pricing per turn, against the cost of a misroute

Input ~350-700 tokens (typed portion + facts + small system), output ~5-10
tokens (enum only, after the client-templated-ack decision).

| | per turn | TTFT |
|---|---|---|
| Haiku 4.5 ($1/$5 per M) | ~$0.0004-0.0007 | ~300-600 ms |
| Sonnet-class ($3/$15 per M) | ~$0.0015-0.002 | ~400-900 ms |
| A misroute | ~$0.02-0.06 raw (full Oracle chain turn) + a wasted premium-billed turn + an honesty violation + usually a corrective user turn | n/a |

A misroute costs 20-100x the classifier call on either model. Precedent for
a per-turn side model call exists and is billed transparently:
`opener_detect` already runs Haiku on every turn with its own tier-'system'
cost line in the digest.

---

## 6. Already built and live (the baseline this plan stands on)

All on the preview branch `preview/controls-pin-2.24.2`, awaiting
cherry-pick to `pricing-v2` (everything EXCEPT the throwaway pin commit
13e48ec):

- **The downgrade counter**: every turn the client staged
  chain_generate/chain_edit that classifyChainIntent downgrades to chat
  logs `[chain-intent-downgrade]` with the typed excerpt AND persists
  `intentDowngrade` per turn, digest-countable. The agreed decision rule,
  recorded in the register: a non-trivial rate means the regex approach is
  the wrong shape - which three-of-three on day one already met. Its typed
  excerpts are the fixture's growth feed.
- **`classificationEntry()`**: the persistence whitelist extracted PURE and
  exported from `_db.js`, with `scripts/test-classification-entry.mjs` (28
  assertions) reading every digest-consumed field back from the BUILT
  entry, opening with the four fields that shipped dark. The rule it
  enforces: a new logClassification field lands in that fixture in the same
  commit, or it should be assumed dark.
- **The noblock honesty tripwire**: `[honesty-violation-noblock]`,
  report-only, catching completion claims in replies with NO block - the
  shape the original tripwire was structurally blind to. First live catch
  was this plan's own incident (19:55:00, honesty=['Done']). Whether it
  should BLOCK (hedge the copy server-side) rather than report is decided
  on its accumulated rate, separately from this plan.
- **ADD_PLUGIN_RE + `scripts/test-chain-intent.mjs`** (ten cases): the
  interim fifth arm that fixes the incident shape today. The register
  forbids a sixth arm without reading the counter first.
- **Register entries** (CHAIN_AI_BUILD_SPEC.md): the five-regexes entry
  (silent chat default, misses wear model-unhelpfulness clothes, decision
  rule above) and the silently-dropped-field entry (four instances in one
  arc; warning comments do not guard rebuild sites, only read-back tests
  do) - the second is why classificationEntry() exists and why
  `intentOverride` must land with its fixture assertion in the same commit.

---

## 7. Build order

1. **Prerequisite:** cherry-picks to pricing-v2, the 2.25.0 release, the
   three version pins (BANDS/CONTROLS/SET_OP_MIN = 2.25.0). Counter starts
   accumulating in production.
2. `/api/classify` endpoint + classifier prompt + the fixture eval harness.
   Model choice decided here (Haiku vs Sonnet on the fixture).
3. Shadow wiring in chat.js: model classifies and logs; regex still routes.
4. Shadow window per section 4; individual disagreement review.
5. Client: split call, provisional bubble, templated ack. The bubble is
   client-owned and never enters replayed history.
6. Cutover per section 4. Regexes to fallback; `intentOverride` primary.

---

## 8. Risk register

- **The classify call is a new client-side failure mode.** If
  `/api/classify` is unreachable, the client sends the main call with its
  own staged label - exactly today's behaviour. Degraded, not broken;
  never blocks a send.
- **Ambiguous as an escape hatch.** Measured (own persisted value) and
  gated (10% rate blocks cutover). Fixture carries non-ambiguous
  neighbours to pin the boundary.
- **Model version drift.** The classify prompt + model id are pinned
  together; a model swap re-runs the fixture eval before deploy, same
  discipline as the cutover gate.
- **Fixture smallness.** Four misses is a seed, not a set. The counter's
  typed excerpts grow it from live traffic; the regression set grows from
  the reply store. The individual-review rule (never counting) keeps a
  small set honest.
- **Two sources of turn-type truth during shadow.** Shadow logs must be
  labelled as shadow; nothing downstream reads them for routing until
  cutover. The 30 May lesson (turnType passed but not persisted) says
  assert the shadow field in test-classification-entry.mjs the day it is
  added.

## 9. Decisions still open

- Exact classifier prompt wording (build-time, gated by the fixture).
- Whether the provisional bubble is replaced by the real reply or folded
  into its first line (client design detail at build time; either honours
  the no-history contract).
- The shadow N (300 stated; revisit only upward).
- Whether the noblock tripwire graduates from report to block (decided on
  its own data, outside this plan).
