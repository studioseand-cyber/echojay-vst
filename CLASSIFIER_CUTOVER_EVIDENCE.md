# Classifier cutover — evidence

A holding file, not a home. The register lives in `CHAIN_AI_BUILD_SPEC.md`
in the **saas** repo (`## Register`); this worktree's copy of that file has
no Register section, and the saas tree is read-only from here. Entries below
are written to be carried across verbatim and deleted from this file once
they land there.

---

## CLASSIFIER ADVISORY, OVERRULED BY THE STAGED LABEL

**3 Aug 2026, live. Plugin, `feat/classifier-client`, v2.25.9.**

At 19:52:58 the user tapped the `channel_mismatch` chip "I'll switch over".
`/api/classify` returned:

    intent = chat
    precondition = (none)
    shortCircuit = false

That verdict is **correct**. Declining to build on this channel is a chat
turn, not a build.

The turn still produced a chain build on the channel the user had just
declined — the exact outcome the `channel_mismatch` short-circuit exists to
prevent. Routing came from the client's staged `turnType`, which fell
through to `chain_generate` via `hadChainFeed`, because nothing routes on
the classifier's verdict until cutover (plan section 4: nothing downstream
reads shadow output for routing).

### Why it is worth recording

It is the counterpart to the morning's two-character regex miss, and it runs
the other way:

| | heuristic | classifier | outcome |
|---|---|---|---|
| regex miss (3 Aug, am) | wrong | would have been right | wrong turn type |
| this case (3 Aug, pm)  | wrong | **was** right | wrong build, on the wrong channel |

The first says the heuristic is fallible. This one says the classifier was
already good enough to have prevented a live wrong build, and could not,
because it is advisory. That is evidence FOR cutover rather than evidence
that the classifier needs more work.

### What was done about it, and what was not

A client-side fix landed ahead of cutover, because the chip produced a wrong
build on every tap until then and cutover sits behind the v2 launch: an ASK
chip with no `intent` now stages `"chat"` explicitly instead of falling
through to `hadChainFeed`. See the commit "The chip that declines a build
stops staging one".

**That fix is a workaround for absent routing, not a substitute for it.** It
corrects one chip's staged label. It does nothing for any other turn where
the staged label and the classifier's verdict disagree, and it will keep
being the reason this particular case looks fine after cutover makes the
general case work.

### Secondary finding: the billing lane

`chain_generate` draws the **premium** lane (monthly pool + credits);
`chat` draws the daily lane. So the wrong staged label also spent a premium
action on a turn where the user explicitly declined to build. Worth carrying
into the cutover argument: a mis-staged label is not only a wrong reply, it
is a wrong charge.

---

## OPEN, NOT CONFIRMED: scoping suppressed after a mismatch question

**3 Aug 2026, 19:54:29.** The follow-up turn ("build a chain for this")
returned `chain_generate` with a preamble and **no** `needs_scoping`, where
the same phrasing fires scoping on every channel in the CLI.

Suspected cause: `suppressScoping` firing with reason `prior_turn_asked`.
`priorTurnAsked` is `/\?\s*$/` — any prior assistant message ending in a
question mark — and the preceding turn was the `channel_mismatch` question,
which ends in "…or will you switch over?".

Why the CLI disagrees: `scripts/try-classify.mjs` defaults `--prior` to
none, so `priorAssistant` is empty there and suppression cannot fire. The
plugin began sending `priorAssistant` in this branch, making it the first
client that can trigger this at all.

**Unfalsifiable from the client.** `suppressScoping` mutates the result
before the response is assembled and sets no `fallback`, so a suppressed
scoping turn and a turn where the model never emitted `needs_scoping` are
byte-identical on the wire. The only record is the server console line:

    [classify-scoping-suppressed] {"reason":"prior_turn_asked","typed":"…"}

in the preview deployment's function logs at 19:54:29.

**Nothing has been changed on the strength of this.** If it is confirmed,
the proposed shape is to key the suppression on `looksLikeScopingQuestion()`
— which already exists in `_classify-prompt.js` — rather than on any "?", so
a mismatch question can be followed by a scoping question while the
yes-please loop stays killed. Narrowing pushes toward firing more often, and
`classify.js` states the risk direction explicitly (suppressing wrongly
costs one unscoped build; firing wrongly asks a user who already said yes),
so it wants an eval run rather than a patch. Saas-side work.
