# Built-in devices — backend (/api/chat) contract

Generalises `SURGICAL_EQ_BACKEND_SPEC.md` from one device to the whole built-in
suite (`BUILTIN_SUITE_PLAN.md` §3). That doc is still correct for the EQ's
`eq_bands` array; this one adds the **universal `params` path** every other
built-in uses, and describes the new shape of the capability advertisement.

As before: the client half is built and authoritative, and lives in this repo.
The work is in the backend that serves `/api/chat`.

## What changed on the client

The `[AVAILABLE BUILTINS]` block is no longer a bare list of names. It is now
**generated from the device registry**, grouped by category, and each device
carries its own parameter schema. One block therefore teaches the model both that
a device exists and exactly how to dial it:

```
[AVAILABLE BUILTINS — EchoJay's own built-in devices, always available regardless
of installed plugins; use in a chain by exact name. Dial one by putting
real-world values under settings_structured.params using the ids listed; any
param you omit is left as it is. The EchoJay EQ additionally takes its bands as
settings_structured.eq_bands]:

-- EQ --
EchoJay EQ
  EchoJay's own fully-parametric EQ, dialled to EXACT values rather than
  approximated. Prefer it for surgical moves: ...
    output_db (dB, -24..24, default 0) - device output trim, applied after the bands
    auto_gain (on/off, default off) - cancel the loudness change the bands cause, ...

-- Utility --
EchoJay Gain
  Exact level trim and constant-power pan. ...
    level_db (dB, -60..24, default 0) - output level; -60 is silence, 0 is unity
    pan (-1..1, default 0) - stereo position: -1 hard left, 0 centre, +1 hard right ...
EchoJay Phase Invert
  Flips the polarity of either channel independently. ...
    invert_left (on/off, default off) - invert the polarity of the left channel
    invert_right (on/off, default off) - invert the polarity of the right channel
```

**This block is the whole contract.** It is generated from the same `ParamSchema`
objects the client validates and applies with, so the server never has to carry a
hard-coded copy of any device's parameters, and the two cannot drift. A device
added in a later wave appears here automatically — the backend needs no release
to support it, and no version pin on either side.

## The two move shapes

### 1. Flat `params` — every device
```jsonc
{
  "name": "EchoJay Gain",
  "role": "...",
  "settings": "...",
  "settings_structured": {
    "params": { "level_db": -3, "pan": 0.5 }
  }
}
```
- **Canonical ids and real-world units**, exactly as advertised. Never normalised
  0..1 — the whole point of a built-in is that it lands on the number you sent.
- **Merge semantics**: any param omitted is left as it is. So "make it punchier"
  can send only `attack_ms`/`release_ms` without restating the device.
- An id outside the device's schema is **reported, not guessed at** — it comes
  back in the summary as `ignored <id>`, and the slot is marked partial.

### 2. Array forms — structured devices only
The EQ keeps `settings_structured.eq_bands` exactly as
`SURGICAL_EQ_BACKEND_SPEC.md` documents it (including a bare `[...]` array, which
must keep working forever). A device can carry both in one move: `eq_bands` for
the bands, `params` for the device-global knobs.

Resolution order inside a move is the device's own and is deliberate — for the
EQ: `eq_preset`, then `eq_settings`/`params`, then `eq_bands`, then `eq_action`.

## Backend changes

1. **Parse the advertisement, don't hard-code it.** Read device names, categories
   and param schemas out of the `[AVAILABLE BUILTINS]` block, and offer a
   built-in only when it appears there. That is what makes the capability gate
   the client's statement rather than a version guess.
2. **Teach the schema.** Each param line already carries id, unit, range, default
   and a one-line description. Pass them through; they are written to be read by
   the model.
3. **Validate and clamp server-side** against the advertised min/max before the
   move ships, and drop ids the device did not advertise. The client clamps too
   (and reports what it ignored), but a move should be sane before it leaves.
4. **Prefer built-ins for exact work.** They dial precisely; third-party plugins
   go through the approximate anchor-table path. Reach for `EchoJay Gain` for
   level/pan staging, `EchoJay Phase Invert` for polarity problems, and the
   `EchoJay EQ` for surgical tonal moves.
5. **Booleans** may be sent as JSON `true`/`false` or as `1`/`0`; the client
   accepts both, plus the strings `"on"`/`"off"`. Prefer real JSON booleans.

## Acceptance

"Drop the level 3 dB and push it slightly right" places an **EchoJay Gain** whose
`settings_structured.params` is `{"level_db": -3, "pan": 0.25}`, and the client
dials exactly those values — verifiable on the device's readouts, with no anchor
map involved.
