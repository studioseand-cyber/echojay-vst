# A → B: status frames during a run's wait (Sean's 15:28 request, 21 Oct 2026)

> *"Under the "Thinking..." line, show the latest status text B sends during the wait (B is adding
> status frames, so agree the shape with B), replacing the previous one as each arrives. It's one
> line, in the same muted style as agent-off's progress lines. It disappears when the next step
> appears or an ask is raised. With no status yet, just "Thinking..."."*

This is A's half, written so B can send against it. **A will read exactly this and nothing else**; if
B's shape differs, say so and A changes rather than both sides guessing.

## The frame

**Matched against B's `echojay-saas/docs/CONTRACT_status_frames_2026-10-21.md`, 21 Oct.** B's version
adds the SSE event name and narrows when a frame is sent; both are taken here:

```
event: status
data: {"type":"status","text":"Choosing the compressor","round":3}
```

- the **SSE event name is `status`** (B's half; A reads the event by name and ignores an unknown one);
- B sends one only when the phase CHANGES - never on a timer, never the same line twice running - so
  A does not have to de-duplicate;
- `round` is sent on every frame, so a late one is dropped rather than drawn over a newer phase.

ONE DIFFERENCE, AND SEAN'S RULING DECIDES IT. B's doc says to show the text *"in place of the generic
"Working…" line"*. His 15:28 words are *"under the "Thinking..." line"* and *"Keep "Working" top right
as well"* - so the status is a second, muted line UNDER "Thinking…", and "Working" stays where it is.
That is presentation, not shape: no field changes either way.

- `text` — one short line, **in B's own words**, already fit to be read by a person. A does not
  rewrite it, truncate it to a sentence, capitalise it or add punctuation; it draws what B sent
  (elided only if it does not fit the card's width). So B should send "reading the rack", not
  "TOOL_CALL: look(rack)".
- `round` — optional, and only used to drop a frame that arrives after the round moved on. Absent is
  accepted: A then trusts arrival order.
- No id, because **there is nothing to update**: each frame replaces the last one. A status is not an
  entry in the run's history and is never recorded — it is the current word about a wait.

## What A does with it

1. It is drawn **under the "Thinking…" line**, as one muted line, in the same style agent-off's
   progress lines use.
2. **The latest wins.** Each frame replaces the previous text; no list, no scroll, no history.
3. It is **cleared** the moment the wait it describes ends:
   - the next step appears, or
   - an ask is raised (the run is waiting on Sean, and his buttons are what he is looking at), or
   - the run finishes, stops or fails.
4. **With no status yet, the line is just "Thinking…"** — A never invents a status, and never keeps
   showing the last round's word into the next round's silence.
5. It is never stored. On a reopen the card shows "Thinking…" with no status until B sends the next
   one, because a status is about a wait happening now and a remembered one would be a claim about
   the present made from the past.

## Where it lives in A

In the chat-log round (his instruction): the run's own log entry carries `statusText` in its payload,
written by the live run and cleared by the same three events above — so the card is **drawn from the
entry**, not patched onto a view. Nothing about this is in the view-rebuild path, which is what the
chat-log plan is removing.

## What A will NOT do

- No status without a `text` (an empty frame clears nothing and shows nothing — it is ignored, and
  the log says one was ignored).
- No status drawn while an ask is up, even if one arrives late.
- No status on a card that is history (a reopened finished run).

---

## Appendix: the limiting budget's names, reconciled (21 Oct 2026)

Checked against B's `CONTRACT_limit_budget_2026-10-21.md`.

**Matched, unchanged:** `dial_on_playback.avg_limit_db` and `peak_limit_db` (a JSON `null` or an
absent field reads as "that half is not bounded", which is also how B's "push anyway" lifts the
budget); `stoppedAtBudget`; `reachedLufs`.

**Changed on A's side:** B reads **`avgLimitDb` as the MEASURED limiting** (its own example is
`"avgLimitDb": 3.1` against a budget of 3). A had sent the measurement as `limitAvgDb` and echoed the
budget back as `avgLimitDb` - two names a word-order apart meaning different things, which would have
delivered a plausible-looking wrong number. A now sends the measurement as `avgLimitDb` /
`peakLimitDb` and **does not echo the budget**, since B sent it. A also fills B's optional `inputDb`
and `slot` when the budget held the push, because that is what B's "push anyway" arithmetic adds to.
