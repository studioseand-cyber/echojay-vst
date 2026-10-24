# Morning report — 25 Oct 2026

## The one line at the top

**The build you have is View only.** Control through the live view is built, proven on the wire and
guarded, but it ships **off** — your ruling of 22:1x, and I have not overridden it: the CL 1B
persistence proof needs you at the Mac, so it could not be done overnight. Turning it on is one
line once that proof is green.

**Nothing was installed over your night's work without the gate passing.** _(build section below.)_

---

## 1. What last night's loss actually was

Your settings were overwritten by the **borrow session's close-apply**, not by control:

```
22:09:05  EJApply: slot 1 "Tube-Tech CL 1B" hadState=N seeded=N -> withheld=N edited=Y action=COMMIT
```

`hadState=N seeded=N` means the Link's own state for that slot never arrived, so the V2's mirror ran
**defaults**; `edited` is "current differs from the baseline", the baseline was empty, and differing
from nothing read as a change you had made. It committed those defaults onto the Link.

It fired twice, and the one healthy decision in the log is the contrast that proves it:

| time | state arrived? | verdict |
|---|---|---|
| 21:30:43 | `hadState=N seeded=N` | **COMMIT** — defaults written |
| 21:32:58 | `hadState=Y seeded=Y` | LEAVE-UNEDITED — correct |
| 22:09:05 | `hadState=N seeded=N` | **COMMIT** — defaults written |

Control itself worked: `slot 1 "Threshold" set to 0.3650 ("-11.7") … this Link now holds it`, then
0.5050 ("-18.7"). You heard it because it was real. The commit then wiped it.

### The fix, in your words

> *"commit only if the slot was seeded from the Link's state, or born in this V2 session with a known
> identity. Never commit a copy that is neither."*

- `withheld` no longer carries the `hadState` qualifier. **Not seeded → never written back**, under
  its own verdict name (`LEAVE-UNSEEDED`) so the log says which of the two reasons it was.
- **Born-here is recorded at the add**, not guessed at the commit: a picker add, and the deselect
  and post-ack re-bases, mark their records. At the commit a slot you placed and a Link slot whose
  state never arrived are indistinguishable on every other field — which is precisely why the
  settings were lost — so the difference is written down when it is still known.
- The post-ack re-base said "created here **(or unknown)**" and committed both. That conflation is
  the hole; it now needs the created identity as evidence, and an unknown origin is treated as the
  Link's and left alone.

### The agent and picker roads are unaffected, and I checked rather than assumed

- **The agent's value steps never touch this road.** They go as chain-cmd v2 editOps straight to the
  Link — your 21:32 log: `EJLink: chain-cmd v2 -> 4814a16004 … source="agent"` then `applied set@1`.
- **A picker add** sends an `add` as a chain-cmd and gets a record marked born-here, so its later
  edits still commit. SH18d exists specifically so this fix cannot silently break adding a plugin.

---

## 2. The empty rack — the answer you asked for

**The Link restored its rack from the session, and the session had no rack in it.** The 0 was a real
empty rack, published 18 seconds after the restore — not an early publish racing a restore.

```
22:08:18  EJScan: cache loaded, 2426 entr(ies)                     <- the new process starts
22:08:25  EJLinkState: setState uid=4814a16004 (foreign/seed) chain=0 slot(s) in the chunk;
                       host has 0 slot(s) now                      <- THE ANSWER
22:08:25  EJLinkState: registry snapshot (state restored) uid=4814a16004 name="Aitch_4_01"
22:08:43  EJBorrow: fresh sidecar read for 4814a16004 -> valid, 0 slot(s)
22:08:49  EJBorrow: the picker's add on 4814a16004 carries a base of 0 slot(s) FROM THE LINK'S
                    SIDECAR - the mirror says 1, and the mirror is not the authority
22:08:49  EJLink: applied add count=0->1 model=1 lease=1 seq=1792876129 applied=1/1
```

`chain=0 slot(s) in the chunk` is Logic handing the Link its saved state with no chain in it. The
Link's save path **does** write the rack (`obj->setProperty("chain", chainModelToVar())`), so a chunk
with no chain means **the chunk was saved before the CL 1B existed on that Link** — the project was
not saved between the add at 21:30 and the restart at 22:08. Restarting Logic for a new build does
not save the project.

`(foreign/seed)` on that line is a **labelling artefact, not a finding**: the "own chunk" test needs
the registry index, and the registration lines come immediately *after* the setState line, so the
index was still -1 when it printed. The chunk's uid was this Link's own.

**What the log cannot tell us:** whether you saved. `LinkProcessor::getStateInformation` logs
nothing, so a save leaves no trace at all. One line there — the chunk's slot count at save time —
would make this answerable next time instead of inferred. **Proposed, not built**, since you asked
for diagnosis only.

---

## 3. The read-back that said it could not confirm

Approved fix (b), done. Your 21:32 figure was never waited for at all, and the chain was three deep:

1. the read-back's only road to "the dial is still running" is the sidecar's `dialPending`;
2. a dial write bumps the hosted-parameter epoch, whose publish goes through the **settle** path, so
   that fact went out late;
3. and the confirm road calls its re-read with a budget of **one look** — so without `dialPending`
   it never looked again.

Fixed at the cause: the dial's state is now **its own publish trigger** (a fingerprint of every
slot's pending and confirmed flags, compared on each publish, allowed to bypass the settle). The
look-budget arithmetic was extracted so a guard can hold it: with no pending reported the budget is
one; with pending it is twenty-five looks at 800 ms.

**Also fixed, from the same test:** the dial's *confirmation* was only published when the slot had a
measured input level, so with nothing playing it never went out at all, however settled the dial was.
That line now sits beside `dialPending` where it belongs. That is why your morning test passed and
your evening one failed — audio was playing in the morning.

---

## 4. Everything else from the night

- **The "meters disagree" rule**, held from your 09:10 ruling, is in: disagree now means opposite
  sides of the band or more than a stated margin apart, and two readings both short means **step**.
  Both runaways the old gate was built for are opposite-sides cases and stay held.
- **Two design notes, no code**: `docs/LIVE_VIEW_LAYER3_2026-10-25.md` and
  `docs/SLOT_METERS_2026-10-25.md`. Each states what it costs on the audio thread (layer 3:
  **nothing**, 0.7–0.9 ms per second on the Link's *message* thread, measured on your CL 1B; slot
  meters: **nothing new at all** — those tallies are already taken on the audio thread for the
  sidecar) and how each fails safe. Both end with the same recommendation: **slot meters before
  layer 3**, because layer 3 insures against a fault no plugin in your rack has been shown to have.

---

## 5. Your test steps, in order

**Do the first one on a COPY of a session.** It is the test that lost settings last night, and
although the cause is fixed and guarded, the proof is what you are about to do.

### Test 1 — the CL 1B persistence proof (this is the one that gates control)

1. Open a **copy** of a session that has a Vocal Link with the Tube-Tech CL 1B on it. If the rack
   comes up empty, add the CL 1B through the V2's picker, then **save the project** — that is the
   step whose absence explains last night's empty rack.
2. Set the CL 1B's Threshold to something you will recognise — **-20** — on **the Link's own window**.
3. Select that slot in the V2 so the live view comes up. Confirm the view shows **-20**, not
   defaults (that is layer 1, and it passed at 20:40).
4. **Close the V2 window.** Then open the Link's own window again.
5. **Expect: still -20.** Last night this is where it became defaults.
6. In the log, expect `action=LEAVE-UNSEEDED` or `LEAVE-UNEDITED` for that slot — never `COMMIT`.

If that passes, control can be turned on: it is one line, and the four guards are already green.

### Test 2 — a picker add still lands (the fix's other side)

1. On a Link's rack, add a plugin through the V2's picker.
2. Change one of its controls in the V2.
3. Deselect the rack, then reopen it. **Expect your change to be there** — a slot you placed is
   legitimately newer than the Link and must still commit.

### Test 3 — the read-back confirms

1. With audio **stopped**, ask the agent to set the CL 1B's Threshold to -20 on a Link.
2. **Expect a confirmed read-back** with a figure (about -19.8; that is the knob's resolution, not a
   miss), rather than *"I could not read these values back to confirm them"*.
3. Then repeat it with audio playing. Both should confirm now; only the playing case did before.

### Test 4 — the live view, unchanged from last night's good parts

Select the CL 1B with audio running: the picture and the meters should move, and turning a knob on
the **Link's own window** should move it in the V2's view within a moment. The shield should read
*"View only - change settings on the Link for now"*, because control ships off in this build.

---

## 6. Build, gate and guards

_(filled in below when the overnight gate finishes.)_
