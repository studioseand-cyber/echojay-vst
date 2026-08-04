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

## THE SAME CHIP AGAIN, WITH THE CLIENT LABEL ALREADY HONEST

**3 Aug 2026, 20:29:00. Plugin, v2.25.10 (`01c74bd` installed).**

Second tap of "I'll switch over", after the client-side fix. `/api/classify`
returned `intent=chat, precondition=(none), shortCircuit=false` — correct
again — and this time **the client staged `chat` as well**. The build still
happened.

### The arm

`classifyChainIntent` (`api/_prompt-shapes.js:212`) runs independently of the
classifier, on the raw message, and overrode it:

```js
if (ANSWER_TAP_RE.test(typed))
  return (clientTurnType === 'chain_edit' && hasCurrentChain) ? 'chain_edit'
                                                              : 'chain_generate';
```

`ANSWER_TAP_RE` is `/\(answering:\s*["“][\s\S]{1,200}["”]\)\s*$/`. The two
arms above it were checked and neither matches: `EDIT_REQUEST_RE` wants
`remove|swap|replace|move…` plus a device noun ("switch" is not in the verb
list), and `ADD_PLUGIN_RE` wants `add|load|insert|put on`. The build verb and
the word "chain" inside the quoted question are a red herring —
`CHAIN_REQUEST_RE` sits *below* the answer-tap arm and is never reached. The
arm fires on the tap FORMAT alone.

### Why this is general, not a classifier-chip problem

There is no staged value that yields `chat`:

| staged | resolved |
|---|---|
| `chat` | `chain_generate` |
| `chain_generate` | `chain_generate` |
| `chain_edit` + rack present | `chain_edit` |
| `chain_edit`, no rack | `chain_generate` |

Every chip tap from BOTH clients uses the `(answering: "…")` form — the
comment above the regex says so, and `ASK_PROTOCOL_NOTE` in chat.js requires
it. So the arm treats every tap as a build regardless of what the chip
meant. It predates the classifier and applies to reply-ASK chips too; the
classifier's mismatch chip is simply the first chip whose meaning is
explicitly "no".

The arm's premise was true when every ASK came from a chain-offer reply: a
tap WAS where the deferred build happened. The classifier broke that premise
by producing ASKs whose chips can decline.

### No client-side fix can hold

The only client evasion is breaking the wire format the regex anchors to.
That is not a fix: both chip renderers depend on the form, it is the
self-contained Q→A pair that survives history trimming, and defeating it
would also break the case the arm legitimately exists for. It would be
invisible to the saas side until something else broke. **Server change or
cutover.**

### Correction to the first entry: the billing claim

The first entry states the mis-staged label was "a wrong charge". For this
turn shape that is WRONG, and it is corrected here rather than quietly
edited. `chat.js:1653` charges on `reqTurnType` — the SERVER-resolved type
— and `chat.js:1517` says so: *"Everything downstream keys on reqTurnType —
pool (freeV2Lane), model (resolveModel), prompt shape (slimChat)."* Since
the answer-tap arm resolves the tap to `chain_generate`, the premium action
is spent whatever the client stages. The client-side fix does not recover
it. The charge is still wrong; the client just cannot be the thing that
fixes it.

### Proposed server shape (saas-side)

The arm already trusts `clientTurnType` for `chain_edit`. Extending that
trust to `chat` is the minimal change:

```js
if (ANSWER_TAP_RE.test(typed))
  return (clientTurnType === 'chain_edit' && hasCurrentChain) ? 'chain_edit'
       : clientTurnType === 'chat'                            ? 'chat'
       : 'chain_generate';
```

That is only SAFE because of the client fix in `01c74bd`: before it, chips
with no intent staged `chain_generate` via `hadChainFeed`, so a staged
`chat` was not evidence of anything. The two changes are halves of one fix
and the client half is already shipped. Cutover subsumes both — the
classifier's `intent=chat` would route directly and the arm would not be
consulted.

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
