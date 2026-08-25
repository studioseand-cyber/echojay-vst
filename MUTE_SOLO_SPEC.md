# Mute & Solo — EchoJay's own mixer layer (spec, 27 Aug 2026)

Replaces LISTEN. Independent of Logic's M/S, which is never read, never
written, never inferred. STATUS: BUILT AND GATED (27 Aug 2026) — the §8
gate plan runs in linksync_test (composition truth table, fabric,
transport, persistence) and borrowhost_test (honorary strip as
behaviour, LISTEN-deletion pins, contextual-line pins).

## 1. What it is

Two controls, EchoJay-wide:

- **Mute**: this Link silences its own output. A mix decision.
- **Solo**: every *other* Link mutes. A monitoring state. Multi-solo is
  allowed, standard mixer behaviour: the solo set is "every live Link
  with solo on", and a Link is solo-muted iff the set is non-empty and
  it is not in it. Solo subsumes what LISTEN does today; **LISTEN is
  deleted** (the control, `audioOn`'s listen semantics, and the
  listen-solo route fork — not the through-main route itself, which
  survives as the incapable/over-budget fallback, §6.4).

Placement: permanently under the rack in the main (where LISTEN sits
now), acting on the viewed rack; and M/S on every strip in the Link
mixer. Both surfaces render **from the sidecar** — closed-loop display,
never a local echo (the §8.5b lesson: a shown state is a confirmed
state).

## 2. State model: three reasons, one silence

The Link's output gain is driven by ONE composed want:

```
silent = muteUser            (the user's mix decision, this rack)
      || muteSession         (a lease's muteOut — §8, unchanged)
      || muteSolo            (someone else is soloed and I am not)
```

**Composed, never shared** (ruling): three flags, one ramp. The existing
`rackMuteMix_` 30 ms ramp becomes the renderer of the OR; each reason
keeps its own lifetime:

- `muteUser` — set by the user (either surface), cleared only by the
  user. **Releasing a session must never clear it**: the lease
  Release/Expire path clears `muteSession` alone, exactly as it clears
  `rackLeaseMuteWant_` today, and touches nothing else.
- `muteSession` — is `rackLeaseMuteWant_`, unchanged: rides the lease
  file, one lifetime with the bypasses, one restore path.
- `muteSolo` — derived every scan tick from other Links' published
  state; never stored beyond the tick, so it can never outlive its
  cause.
- `soloOn` — this Link's own solo flag. In-memory only.

Mute overrides solo on the same strip (standard mixer): a muted rack
stays silent even while soloed — the formula already says so.

`muteEngaged` in the sidecar keeps its meaning: the output is actually
silenced this instant, whatever the reason. The §8.5b watchdog contract
is unchanged — it asks "is the silence I commanded confirmed", and a
user mute confirming it early is still confirmed.

## 3. Ownership, transport, and the closed loop

Solo and user-mute live **in the Link instance** and are published in
its sidecar (two additive bits, §8 conventions: written only when true,
absent reads false):

- `muteUser` (also drives the strip's M lamp)
- `soloOn` (drives the S lamp)
- `muteSoloCapable` — the capability bit (§7)

The main's buttons are remote controls: a ctrl-cmd per toggle
(`setUserMute {on}` / `setSolo {on}`, `nextCtrlSeq`, consume-and-answer
— a refusal is consumed and acked with its reason like every other
cmd). The Link applies, publishes, acks; the main's lamp follows the
sidecar, not the click.

**The fabric**: each capable Link scans the registry's live rows
(RegLiveness — the same predicate the selector uses, one definition) at
a low rate inside its existing 30 Hz timer, computes `muteSolo`, and
ramps. No coordinator, no solo file: the solo set IS the live sidecars.
Target end-to-end solo latency (click → other channels dip) is under
half a second; the exact scan cadence is a build-time measurement, and
if the sidecar-write path is too slow for it, the fast path is a tMs
bump, not a new channel.

## 4. Persistence (ruling)

- **Mute persists**: `muteUser` rides the Link's saved state
  (getState/setState) like the rest of the channel's mix identity. It
  is NOT chain state: chain saves, CHAINS-tab loads, dashboard recall,
  and a borrow session's Apply never read or write it.
- **Solo does not**: `soloOn` is never serialized, in any state, ever.
  A saved solo is how a project opens silent and nobody knows why —
  ruled out by construction, and the gate asserts the serializer
  refuses it rather than trusting review to keep it out.
- Project reopen therefore restores mutes (lamps lit from sidecars) and
  opens with an empty solo set.

## 5. The honest limit, stated in the UI

Solo only silences Linked channels. Un-Linked channels keep playing —
soloing the vocal still leaves an un-Linked guitar playing, and the UI
says so rather than letting the user discover it. **Amended (27 Aug
2026): contextual, not permanent** — the design fact is not a warning,
and a permanent line explaining it is furniture:

- WHILE any solo is active (and only then): one quiet line — "Solo
  mutes Link channels only - use Logic's solo for un-Linked tracks."
  It leaves when the solo set empties.
- The warning stays a warning: if live rows include Links without
  `muteSoloCapable` while a solo is active, the line extends — "N
  Link(s) predate solo and keep playing" (count from the same liveness
  pass; never counted from dead rows). An old binary ignoring solo is a
  surprise, not a design.

## 6. Interactions, ruled explicitly

### 6.1 Solo × a rack being edited (the LISTEN case)

Soloing while editing is the case LISTEN was built for, and solo DOES
change what in-context monitoring plays:

- **Solo the edited rack** (S under the rack during a session): every
  other Link mutes; the session's own machinery is untouched — the
  edited rack's channel stays session-muted, the main keeps injecting
  the processed edit. The user hears the edit in isolation, to the
  honest limit (plus un-Linked channels). This replaces LISTEN.
- **The injection is an honorary strip** (ruling): the main's in-context
  injection stands in for the edited rack's output, so it follows the
  fabric as if it were that rack's strip — *audible iff the solo set is
  empty or the edited rack is in it*. Someone soloing another rack
  ramps the injection out (30 ms, the existing borrowCtxMix_ ramp) with
  a named banner ("<name> is soloed - your edit is muted with the other
  channels"); it ramps back when the set empties or the edited rack
  joins it. Without this rule the injected edit would be the one
  channel a solo cannot silence — a lie inside the main's own output.
- **Mute the edited rack** (M during a session): legal; the channel is
  already session-silent, and the user mute is what remains after
  release. The lit M lamp is the explanation.
- A solo on a rack someone ELSE is editing needs no special case: its
  channel is session-muted regardless; solo membership only decides
  what everyone else does.

### 6.2 Deleted or stale soloed Link

Solo lives in the instance and is counted only from LIVE rows:

- **Deleted** (channel removed, clean exit): its registry slot releases
  and its heartbeat stops — the next scan tick drops it from the solo
  set and every solo-muted Link ramps back. Worst case one liveness
  window, same threshold the selector already uses.
- **Stale/crashed** (heartbeat frozen): identical — RegLiveness refuses
  a non-climbing heartbeat, the set drops it, everyone recovers. No
  reaper involvement, nothing persisted to clean up.
- The failure mode this kills: a solo that outlives its owner. It
  cannot, because `muteSolo` is re-derived from live evidence every
  tick and stored nowhere.

### 6.3 Solo × session mute composition

All compositions fall out of §2's OR with per-reason lifetimes, and the
gate walks the truth table rather than trusting the prose: user mute
held through a session's engage AND release; session mute lifted while
user mute holds (still silent); solo-mute lifted while either holds
(still silent); every reason lifted (audible again, ramped).

### 6.4 What remains of LISTEN's machinery

Deleted: the LISTEN control, `audioOn`'s listen meaning, the
listen-only solo route fork, and the "LISTEN=solo control" wording in
§5a-R/§8.7 (specs amended in the same commit that builds this).
Retained: the through-main borrow route, exactly and only as the
monitoring fallback when in-context is refused (incapable Link,
over-budget rack, watchdog drop) — its trigger no longer mentions
listen. In that fallback the main's output IS the borrow ring, so
solo/mute of other Links doesn't change what the user hears; the
fallback banner already names the mode and that stays the honest
answer.

## 7. Capability gating

Same convention as every carved bit: a binary that implements the
composed mute, the two cmds, and the fabric scan publishes
`muteSoloCapable: true`; absent reads false.

- The main never sends `setUserMute`/`setSolo` to an incapable row —
  the buttons render disabled with the reason ("this Link predates
  mute/solo - reinstall it"), never a silent no-op.
- An incapable Link neither publishes nor scans: it keeps playing
  through anyone's solo, and §5's UI line counts it.
- Old main × new Link: the old main has no buttons and never asks for
  the new keys — additive fields, nothing to gate.

## 8. Gates (functional, both suites, before any push)

- **Composition truth table** through the REAL processBlock (linksync):
  each reason alone silences; each pairwise hold survives the other's
  release; user mute survives session release (the ruling, asserted as
  behaviour); all-clear restores signal. Level-asserted, ramped-block
  aware, like the §8 mute arm.
- **Fabric** (linksync, two Link instances, one shared dir): B publishes
  soloOn → A's scan silences A's output; B's heartbeat freezes → A
  recovers within the threshold; B releases its slot → same; A solos
  itself too → A stays audible (multi-solo membership).
- **Persistence**: state round-trip carries `muteUser` and provably
  never `soloOn`; a chain save/load and a session Apply leave
  `muteUser` untouched.
- **Transport** (consume-and-answer): `setUserMute` cmd → consumed,
  acked, sidecar shows the bit; the same cmd at an incapable target is
  never written (main-side refusal pin).
- **Honorary strip** (borrowhost, level-asserted): live session
  in-context, foreign solo appears → injection ramps to silence with
  the banner recorded; edited rack joins the solo set → injection
  returns.
- **LISTEN deletion**: the control and route fork are gone (code-form
  pins), and the fallback trigger is proven live without them
  (functional arm through the refused-capability path).

## 9. Out of scope, named

- M/S on the Link's own editor (the Link user muting themselves) —
  natural follow-on, same flags, not in this pass.
- Solo-safe (exempting a Link from solo-mutes) — not asked, not built.
- Any interaction with Logic's own M/S — permanently out; the layers
  never see each other.
