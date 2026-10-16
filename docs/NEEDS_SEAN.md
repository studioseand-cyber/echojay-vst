# NEEDS_SEAN — decisions taken without you, and the few things a machine cannot judge

**16 Oct 2026.** Your rule: *"Anything you can't judge goes in NEEDS_SEAN.md with a PNG and one line.
Decisions go at the top, but take the most reasonable reading and keep going."* So the decisions are first,
each with the reading I took. Nothing here is waiting on you; if you disagree with one, say so and I will
turn it round.

---

## Decisions I took

### D1. A typed reply when there ARE choices on screen is a NEW INSTRUCTION, not an answer

Your blocker said *"a typed reply must never be silently dropped"*; your #21 said *"a new instruction
closes the waiting run (marked stopped) and starts a new run."* Both are now true, told apart by whether
the ask has anything to tap:

- **No choices at all** (the 09:19 case) — typing IS the answer, whatever `allowFreeText` says.
- **Choices on screen and the text matches one** — it is answered as a tap.
- **Choices on screen and the text matches none** — it is a new instruction: the waiting run is stopped and
  the text starts its own run.

Nothing is dropped either way, which is the rule both of your instructions share.

### D2. The rig will not load MTransient, because it is licence-bound

MTransient's bundle carries `__Pace_Eden.bundle`. Your standing rule is that nothing of mine loads a
PACE/iLok plugin (after the 2 Oct security prompt), and the rig now refuses one by name even if it is asked
for. So the matrix's "map exists but is wrong" case (P3) **cannot run in the rig** and is on the Logic
smoke-test list for you instead. The rig's third-party plugins are **Renaissance Axx** and **Vertigo
VSM-3**, neither of which is wrapped - and Renaissance Axx is the one you watched land at -20.0, which is
why it is the read-back proof case.

### D3. Links run in their own processes for channel scenarios, in process for audio scenarios

A separate process is the only way to prove the V2 is not reading a Link's memory, and three of this week's
faults lived exactly there. But Logic renders out-of-process AUs from ONE audio graph, so its V2 block and
Link block belong to the same render pass, while two rig processes pump their own blocks on their own
clocks. So: channel, lock, window and rack-view scenarios run on the two-process setup; audio, meter and
level scenarios run in process, where the block interleave is exactly Logic's. Every row in the report says
which. Full reasoning in `docs/RIG_LINK_CAPABILITY.md`.

### D4. "WORKING" leaves the tile when there is a GR figure

Your #16: *"fit it or drop the word."* "GR 10.9 WORKING" is fifteen characters in a 118 px row and never
fitted. A GR figure only appears when the device is reducing something, so the word says nothing the figure
has not already said - and it is the word that goes. HOT stays: four characters, and it is a warning.

### D5. The GR result carries no instant or single-transient figure at all

Your #17. A figure that must not be quoted has no business being sent, so `grNowDb` and `grSpikeMaxDb` are
gone from the result. `grPeakDb` and `peakDb` now **alias the loudest hits** (the 95th percentile), because
a prompt written against the old contract looked for a "peak" and found the transient. There is also a
ready-made `sentence` the model can quote.

### D6. Suggested Settings shows the landed value, or says it is not confirmed

Your #20, where the card and the step contradicted each other. The card was composed from the value that
was ASKED for; it now reads the plugin's own landed display where there is one, and labels the other case
"(sent, not confirmed)". For the record, in your Pro-C 3 case **the card was right and the read-back was
wrong** - the log has Pro-C 3 reporting "-18.00 dB", "4.00:1", "9.911 ms" from its own display.

### D7. After Undo all, the closing is replaced rather than edited

Your #14. Nothing on my side can know what the rack is now, only that what the closing claims has been
undone - so the sentence becomes *"Everything this run changed has been undone, so what it said above no
longer describes the rack."* The server's original closing is kept in the record, not shown.

### D8. A read-only run's control says "N steps"

Your #18, taking "show 'N steps'" rather than "no count": a run with four steps still has four steps worth
collapsing, and the control has to say what pressing it will do.

### D9. One scenario cannot be red against an old bundle, and the report says so

E7 (the GR meter's two labels never overlapping) is an assertion on a pure layout function that is compiled
INTO the runner, so when the runner runs against ship_2026-10-16a it still tests today's geometry. It is
marked in the report rather than counted as a reproduction. Everything else starred runs red against 16a.

---

## What the new runner found on its FIRST run, before any scenario

**Two EchoJay instances starting at the same moment can end up on two different channels, and neither
can see the other.** This is a real defect in the shipping build, not a rig artefact, and it is exactly
what the out-of-process Links were built to expose.

`LinkShm::resolveDir()` decides where the shared directory is by writing a probe file and then deleting
it. The probe was called `.probe` - **the same name in every process**. Two instances starting together
raced: one process's delete landed between the other's open and write, the write "failed", and that
instance quietly took the TMPDIR fallback instead. From then on it had its own registry and published its
rack where nobody was looking.

The proof, from the rig's own run: the Kick Link registered (`uid=45d483f290`, audio flowing, rack
published) and the V2 never saw it, because its `rack-45d483f290.json` was in `$TMPDIR/echojay_link/`
while the V2's was in `<state root>/Library/Application Support/EchoJay/link/`.

**In Logic this is the "my Link doesn't show up" shape:** several EchoJay instances initialise at once
when a project loads, and whichever loses the race disappears from the rack list. I have not seen it in
your logs, but the window is milliseconds wide and nothing would have told us - which is the part I have
also fixed.

Fixed three ways: the probe is `.probe-<pid>-<n>` so no two processes share it; a failed probe on a
directory that exists is retried once, because a transient failure is not an unwritable directory; and
taking the fallback is now logged loudly with its reason, so a split channel reads as a line in the log
rather than as a Link that never appeared.

---

## Needs your eyes (nothing is blocked on these)

These are the judgements a check cannot make. Each has its PNG under `docs/review/<date>/<scenario>/`.

1. **Does the run row read well now?** `docs/review/<date>/G6/editor.png` and `G7/editor.png` show a
   finished run with the build bubble gone and the closing only inside the message. The *words* are right;
   whether the row reads as one thing rather than a list is your call.
2. **Is the tile's caption legible at 6.5 pt?** `E2/editor.png`. "GR 4.4" fits where "GR 10.9 WORKING" did
   not, but I cannot judge whether you can read it at a glance on your screen.
3. **Does the ask hint read as help rather than as an error?** `B1/editor.png` - the amber line under a
   choice-less question: *"Type your answer in the box below and press Enter."*
4. **The mode hint's three seconds** - `G3/editor.png` catches it mid-life. Whether three seconds is long
   enough to read is a human judgement and always was.
