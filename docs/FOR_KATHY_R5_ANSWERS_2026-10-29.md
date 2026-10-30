# A to K, answers to round 5 (29 Oct 2026)

Sean's order tonight puts the replace regression and the EchoJay-device knobs first; both are now
proven in the one-process configuration. These five answers are decided and are implemented in that
order behind his items 3 to 5. Where a decision was mine to make, it is made here and the reason is
given, so you can code against it before it lands.

## R5-1. `hasCap ("match_plan")` — one switch, and it is the server's

**Decided: `hasCap(name)` reads `features.<name>` from `/api/plugin/config`, and nothing else.**

So `hasCap("match_plan")` answers `features.match_plan`, which is the same switch
`POST /api/plan/draft` checks for kind `"match"` (B's `b/plugin-config` 5cd2e10; 404 `not_enabled`
when it is off). That is your rule and Sean agreed it: one switch per feature, named as its
capability, read by the route behind it.

**It is not hard-wired, and that is the point.** A `true` compiled into the build would let the
button appear while the route refused, or the reverse, and the pair would then disagree in the one
situation nobody tests - a half-rolled push. The switch is off until C7's push 3 is live, so the
page will still not offer "Make a plan" on the next build; the difference is that it will offer it
the moment the server says so, with no new plugin.

`real_editor` and `live_view` keep answering as they do today (they are facts about this build, not
server features). `match` stays accepted as a FAULT-REPORT feature name, which is B's note, and is
not a capability.

## R5-2. The reference's library id — **on `ReferenceResult`**

**Decided: the id is carried on `ReferenceResult`, as an appended field `libraryId`. No
`selectedReferenceId()`.**

Why that way round, since you offered both: the id then travels with the thing it identifies. Every
road that hands out a `ReferenceResult` carries it - `selectedReference()`, `getReferences()`, a
restored one, a moved one - and there is no second call that can answer about a different reference
than the one the page is holding. A `selectedReferenceId()` is a parallel lookup, and a parallel
lookup is a thing that can disagree: the page would have the measured result from one call and the
id from another, with a rack-switch or a browser selection in between.

- `libraryId` is the library's `RefEntry` id, not a hash of the path, so **a moved file keeps its
  id** - which is the half `echojay::matchReferenceId` cannot do and the reason you asked.
- **Empty when the reference is not in the library**, exactly as you specified. Absent, never a
  placeholder.
- Appended, so your existing reads are untouched.

Keep sending `matchReferenceId` until it lands; switch the block to `libraryId` and fall back to
nothing (not to the hash) once it is there, or `match:hist` will key two ways for one file.

## R5-3. `hostBlockSize` — on `CaptureSnapshot`, and on the live buffer

**Decided: `int hostBlockSize` appended to `CaptureSnapshot`, set when the capture STARTS**, which is
the only moment it is a fact about that capture: the host may change it afterwards and a figure read
at the end would describe a different run than the audio did.

- `0` means unknown (a snapshot from an older session, or a capture whose start we did not see).
  **Your rule holds: omit the field, never send 0** - C2's 16..8192 cannot admit it and a clamp would
  invent a number.
- The Live buffer carries the same field, set when the rolling accumulator starts keeping hops, and
  **reset to 0 if the host's block size changes mid-roll** rather than reporting the first value for
  audio that was not measured at it. A live window spanning a block-size change has no single
  answer, and saying so is better than averaging two.

## R5-4. The Link archive compile — **done**

Fixed in `746dfae2`, and `feat/08c` is pushed (origin was four commits behind; it now has R4-1 and
the whole 29 Oct batch). Your gate should build `EchoJayLink_AU` and `EchoJayLink_VST3` again. Both
Link bundles built clean here tonight.

## `postToSessionChat ("match_report", payload)` — becoming real

It is A3's road and it is next after Sean's items 3 to 5. Until then it does what its comment says:
logs the call and posts nothing, so "Report in chat" will appear and no card will arrive. When it
lands it takes C2 section 4's payload with `crestLog` in C2's shape,
`{ v: 1, hostBlockSize?, sides: { reference, mix } }` - the shape you have already moved to, so
nothing on your side changes when it starts working.

`draftPlan` keeps its refusing stub and its sentence until the plan road exists; your SafePointer
handling of a late callback is right, and the callback is always made exactly once.
