# Link Stage 1 — the test card

**Nothing here has ever run in a DAW.** Stage 1 is code-complete; three fixes
from the last round have never executed in a host. This card is the order to
find that out in, taken from `LINK_CHAIN_EDITING_HANDOVER.md` and adjusted for
what the merge changed.

Work top to bottom. **Stop at the first genuine failure** rather than
collecting several — each test's readability depends on the ones above it
passing.

Keep a Claude Code session open alongside, but don't let it fix anything until
a test actually fails. Bring it the observation, not a theory.

---

## 0. PREFLIGHT — the merged binary, before any Link work

First run of a binary where the dashboard and the audio branch coexist.

| # | Do | Pass looks like |
|---|---|---|
| 0.1 | Open the Dashboard tab | Webview loads, signed in, populated |
| 0.2 | Load a chain from the dashboard onto a **non-empty** rack | **Exactly one** confirmation, naming the plugins it will destroy, as the ask shelf — not a modal |
| 0.3 | Recall a chain from chat | One ask; it asks *where to load*, not about replacement |

If 0.2 shows two asks, or none, stop. Two means the consolidation missed a
path; none means the choke point isn't guarding that entrant. Either is a merge
defect, not a Link defect.

---

## 1. THE ENGAGEMENT CHECK — before every audio test below

This is not a test. It is the thing that makes every audio result readable, and
it is repeated at the start of each audio test.

1. Sweep the local copy's control. **The channel must change.**
2. Bypass the local copy. **The channel must go dry.**

Both must hold. If the channel stays wet with the local copy bypassed, the Link
never bypassed its slot, you are listening to its own processing, and every
result below it is a false pass.

---

## 2. TEST A — is the rack view painting truth or intent?

**This gates everything. Run it first.**

Kill the Link mid-add. Then look at the Chain tab's rack view.

- **Pass:** the block does not render, or renders as unreachable.
- **Fail:** the block renders anyway — the view is painting its own intent, not
  the Link's state, and every guard test below becomes unreadable, because you
  cannot tell a real write from a hopeful one.

## 3. TEST B — identity by descriptor, not display name

The screenshot that started this work showed **AMEK EQ 250** while the
human-verified ejmap entry is for the **200**.

Load a chain containing a plugin with a near-twin in your collection. Confirm
the plugin that opens is the one the descriptor names, not a name-similarity
match.

**Why it is this high in the order:** a wrong resolve now sits upstream of
remote editing, where it pulls the wrong plugin's state and applies it back.

---

## 4. THE GUARDS — they write to a rack and have never run

Engagement check first, each time.

**4.1 `baseSlots` on insertion.** Start an edit. Insert a plugin *above* the
edited slot on the Link, shifting its index. Apply. **Expect refusal.**

**4.2 `baseSlots` on removal.** Same, but remove a slot above. Apply. **Expect
refusal.**

**4.3 The 256KB cap.** Edit a plugin whose state exceeds
`kApiStateMaxSlotBytes` (262,144 raw bytes). **Expect refusal, Link-side,
before encoding.**

**4.4 The cap from below.** A plugin just under the cap. **Expect success** —
this is what proves 4.3 is a threshold and not a blanket refusal.

---

## 5. THE RUDE PATHS

Decide the expected result **before** each one, or you will rationalise
whatever happens.

**5.1 Close the main window mid-edit.** Wait past 3s. Reopen. The lease is
processor-held, so nothing should change. **Listen during the wait, not only
after.**

**5.2 Remove the main from its track mid-edit.** Expected: 3s expiry plus one
poll. Not 100ms. Write the number down first.

**5.3 Force quit Logic mid-edit, reopen.** Read by **content**: is audio
actually passing through the slot? A stale lease id must never re-engage, and
the UI not showing a session is *not* evidence.

**5.4 Capture exclusion, both orders**, compared against each other. The bug
here is order-dependence, so one order proves nothing.

**5.5 Contended lease** — two mains, one rack.

**5.6 Sample rate or buffer change mid-edit.** The stream path and the local
copy have separate prepare paths.

---

## 6. THE TWO FROM THIS ROUND OF FIXES

**6.1 Internal device edit, end to end.** EDIT THIS PLUGIN on the EchoJay
4-Band Compressor from a Link's rack. This is `82f3621` — stranded on a branch
for days, merged only now, and **never run in a host at all**. Previously it
gave *"No compatible plug-in format exists for this plug-in"*.

**6.2 The four status-line states.** Especially the 5s no-ack:
*"No response from this Link - nothing was changed."* — the arm that never runs
in normal use. Fix 3 has **no automated test**; these four states are hand
checks and this is the only place they get checked.

Note carried from the handover: the failed-pending sweep runs in the **Link-tab
timer**, so on the Chain tab a failure stays on screen until you visit the Link
tab. Not a stale claim, but asymmetric — and this battery is run from the Chain
tab, so expect it and don't read it as a bug.

---

## What "done" means

Stage 1 passes when every test above passes with the engagement check holding
each time. Then, in order:

1. Move the failed-pending sweep off the Link-tab timer.
2. One shared encode/decode pair for **every** state transfer, not just remote
   editing — ChainHost's cache still has its own separate (correct) pairing.
3. Stage 2 (in-context editing), once the drift question has a spec answer:
   what happens when stamps are present but alignment drifts mid-edit. That
   failure is silent and sounds like a mix decision rather than a fault.
4. Pitch correction device #22, which would have hit the same builtin wall
   `82f3621` removed.

## Rules that apply while testing

- Verify what is installed by **content**, never by version number.
- Any step that can silently do nothing must assert that it did something.
- A wait with no timeout is that failure.
- `RackSidecarSlot` is positionally brace-initialised — any new field goes
  **last**, after the other Mac's sidecar-identity fields, or every initialiser
  shifts silently and still compiles.
