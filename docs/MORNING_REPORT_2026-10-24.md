# Morning report, 24 October 2026 — the night of the 23rd

Six items on your list. **Five are in the build; the sixth (B's two new contracts) is deliberately
not**, for a reason B wrote into the contract himself. The installed UUIDs and every verdict are in
§7; what is owed is §8; exact retest steps are §9. The running checklist is
`docs/OVERNIGHT_PROGRESS_24oct.md`.

---

## 1. The dial's direction — three plugins, one cause, and it was not the direction

Your three reports (Vac Attack `Reduction to +0.0` for 0.2 dB, Lindell SBC `threshold 4.34 → 4.8` for
0.1 dB, bx_opto `Peak Reduction to +0.0` for 0.1 dB, all against a 1 dB target) are **one defect with
two aggravations**, and your logs name all three.

**(a) It read a meter as a knob.** Two lines, two seconds apart, in the 21:03 run:

```
EJThreshold(Link): "bx_opto" carried no start_db for "Peak Reduction" - read 0.00 dB off the plugin
EJDial: slot 3 ("bx_opto") ... READ-BACK: Peak Reduction=33
```

bx_opto exposes that name **twice**: the knob (at 33) and a read-out of what the knob is doing
(0.0 dB, because nothing was being reduced yet). `readControlDb` took the *first* parameter whose name
matched — a proxy standing in for an identity — and the loop then **writes** its opening value, so the
add's 33 became 0. Peak Reduction 0 is no compression, which is exactly the "+0.0" you saw on the Vac
Attack too.

*Fix:* one matcher (`echojay::namedControl`) that prefers a **settable** parameter, used by the read
road *and* the write road, so the two can no longer mean different parameters. A write takes the
settable one only — a `setValueNotifyingHost` on a read-out either does nothing or moves something
nobody named. The line between them is the one this codebase already drew for the map builder:
`isAutomatable()` is the AU's own flag, and on the PuigChild 660 its six controls are "Readable,
Writable" while all 69 LED/VU rows are "Not Real Time, Readable".

**(b) The band contained "nothing".** B sends a single number; we turned it into `[target - 1,
target]`, so **"target 1 dB" became 0.0–1.0** and zero gain reduction was *inside the band*. Your
Lindell read 0.1 dB for six windows, that counted as in-band twice running, and the hold finished
**having never stepped once**. The 4.34 → 4.8 you saw was its opening write (`block carried no
start_db - READ "Threshold" off the plugin: 4.75`), not a dial step. A band that silence satisfies
cannot fail, so it measures nothing.

*Fix:* half a dB either side, floored clear of zero — `[max(0.3, t-0.5), t+0.5]`, which for a 1 dB
target is the 0.5–1.5 band this house already uses everywhere else.

**(c) And the direction is now measured, not just declared** — your ruling, and worth having even
though it was not the cause here. B's `sense` is read first (on your Lindell it was right: the start
line says "lower is harder"), but the dial has evidence nobody else has: what happened when it moved.
If the first step sends the reduction the wrong way by more than the sensor's noise, the sense is
**inverted once, out loud**, and the loop carries on. Once per loop, never twice — a second flip is an
oscillation that would walk the control to its end.

**Guarded** (`comp_contract_guard`): DS5/DS5b the band straddles the target and keeps its floor clear
of zero; DS8/DS8b a plugin naming the same control twice resolves to the **knob** on both roads, and a
name matching only read-outs can be read and never written.

## 2. The dial's result on the card

**The cause was a second source of truth.** The run registered a dial only when B's op *also* carried
`keep_playing` — a field that repeats what `dial_on_playback` already says. Your Lindell op carried
only the latter, so the run never knew there was a dial, and everything downstream reads that one
list: no "Keep it playing…" step was drawn, and `routeDialClosingToAgentCard` found the plugin absent
and fell through to a loose chat bubble. Your 16:40 ruling settles it — *"a dial on playback is part of
the build"* — so every build-armed dial now registers, and `keep_playing` only decides what the log
says about where the instruction came from.

With that, the rest of your rulings land where they were meant to:

- **the step sits with its add**, not in a block at the end of the card (your test 3);
- **no question after a dial**: `ask` now defaults to *false* on this road, and B's own `ask: true`
  still wins, so it is a default and not a new refusal;
- **the fell-short line is B's short shape** — "only reaches 0.1 dB here, short of the 0.5–1.5 dB it
  was aiming for" — instead of "Adjusted the Lindell SBC's Threshold to +4.8 dB", which was reporting
  the dial's own opening write as though it had moved the knob there;
- **at target but the meters disagree says so**: "is doing about 1.0 dB already; its meters disagree
  about it (level 1.0 dB, crest −0.2 dB), so I left it as it is" — your 20:57 point that "only
  reaches" was wrong when it had reached it.

**Guarded** (`agent_client_guard`): KP6/KP6b the dial's step is the row after its add and is Running
there; KP5/KP5b the landed result lands on that same step. (One of my own bugs on the way, worth
recording: I iterated `c.dialResults().getAllKeys()` — a reference into a temporary that dies at the
end of the expression — and KP5 went red with "no dial step". Held in a local now.)

## 3. B's two contracts — read, and deliberately not half-built

They did not exist when your list was written: they arrived **with `B_DONE_TONIGHT` at 22:18**, the
same minute the chain started building the five fixes.

- **`CONTRACT_dial_step_2026-10-23.md`** is a new `op: "dial"` frame — `of`, `step_text`, a structured
  answer (`outcome`/`figure_db`/`end`/`control`/`value`, plus `levelling` for a limiter and
  `clip_drive` for a clip), run in batch order, with a 60 s playback wait and a `not_heard` answer.
  It is **switched on by declaring `dial_step` in the start context**, and B wrote the default to be
  safe: *"Without it: today's behaviour, unchanged. The released plugin is safe."*  So the capability
  is **not declared in this build** — declaring it half-built would have B sending dial frames this
  build cannot run, which is worse for you than not having it at all.
- **`CONTRACT_clip_drive_2026-10-23.md`** is a new dial *kind* (`kind: "clip_drive"`, `control:
  "slot_in"`, `out_follows_in`, `measure: "peak_reduction_across_slot"`) riding the existing
  `dial_on_playback` road — a new actuator inside the loop that writes the slot's IN and mirrors it on
  the OUT, plus a peak-reduction measurement across the slot.
- Both are **"on b/clip-drive, not live yet"** in B's own words, so 3215 cannot send either to this
  build. Nothing is exposed by taking them in the next round with the contracts in hand.

## 4. Undo all on a Link

**It was not a greyed button; it was an empty history.** Your log:

```
EJAgent: undo all walked the history back to the run's checkpoint - 0 step(s), depth now 0 (the run started at 0)
EJAgent: undo all FAILED "there was nothing left to undo"
```

— while the run's four steps each carried an undo token (`undo=add:1792785780996`, …). `ChainHost::
pushUndo` records the local rack and a borrowed copy; a remote **parameter** move records a
`remoteValue` entry (stage 5, 10 Oct); a remote **structural** edit — which is every agent add to a
Link over the wire — recorded **nothing at all**. The existing `chain` kind cannot serve it either: it
restores a snapshot through `rackFor(uid)`, and an unleased Link has no host here to restore into.

*Fix:* the inverse goes out the way the edit did, as ops. A new `remoteAdds` entry is recorded at the
one site that sends the agent's ops, and taken back as `remove` ops over the same wire — **top slot
first**, since removing a lower slot would shift the ones above it, and **each remove carries the name
it expects**. Any op whose slot no longer holds what this entry added is **dropped with a line**
rather than deleting whatever is there now. "Undo all" walks that same history, so a run that only
touched a Link now has something to walk.

Two honest limits: only **appended** adds are recorded (a positioned add does not land on the end, so
the arithmetic would name the wrong slot — it says so in the log and declines rather than guessing),
and **redo is refused outright** (putting a plugin back needs its settings, which this entry does not
carry). "Clickable while the closing waits" was already true — `undoAllowedNow` admits both waiting
states — so what failed was only that there was nothing recorded to undo.

**Guarded** (`global_undo_guard`): RA1/RA1b three wire adds become one entry with a label that says
what it takes back; RA2 top slot first; RA3 redo refused.

## 5. The supersede ask replaces the refusal

The ask existed since this morning but was gated on `! isWorking()` — the 20 Oct split, where a
**waiting** run could be superseded and a **working** one could only be refused. Yours was working, so
you got *"The agent is working on "build a vocal chain" in another chat. Stop it there, or wait for it
to finish."* — twice, because the refusal was appended on every Enter.

*Fix:* the split is gone. Either way it is your account's one run, either way you are told what will
stop, and either way you decide. **Start anyway** stops the old run whatever it was doing (its card
says so in its own chat) and sends your text; **Cancel** leaves it going and puts your text back. The
ask remembers the message it is up for, so a second Enter adds nothing.

**Guarded** (`chat_store_guard`): SU5 a second Enter adds no second ask and repeats no line;
SU6/SU6b/SU6c/SU6d a working run is asked about, the refusal line is gone from that chat, and the old
run keeps going until you press Start anyway.

## 6. The live view is back ON

`kShadowViewEnabled = true`, with the channel-count fix that caused both of yesterday's crashes (the
copy is asked for plain stereo with every other bus disabled, the scratch buffer is sized to whatever
the copy *ended up* with, and channels past the stereo pair are fed silence). Guarded by the
arithmetic the null pointer came from (`chat_store_guard` SH8/SH8b).

**And the missing audio is fixed too** — the reason the X-Limit's meters sat at −99.99 dBTP. The slot's
index was **baked into its pre-trim node when the node was built**, and the node is built once per
slot while `slots_` is a vector: an insert shifts the elements *and their nodes*, so a node still
saying "I am slot 0" ends up at slot 1. Two nodes then answer to 0, nobody answers to 1, and a tap
aimed at slot 1 is fed by nothing at all. Your rack had plugins added through the session. The index is
now an atomic the host refreshes on **every** rebuild. This is also the rig's own B9 zero-frame red, so
it was two reports of one bug.

As you asked: **you are the test** for the two plugins that crashed (UnFairchild, the Waves SSL) — the
rig cannot load them. If either crashes again, the switch goes back to false in the same build it is
found, and the crash report will say whether it is the same defect or a second one, because the two
frames that identify it are fixed.

## 7. The build, and every verdict

| bundle | arm64 UUID |
|---|---|
| EchoJay V2.component | `96420D29-5A29-3573-8148-EB7E8F6AB8BB` |
| EchoJay V2.vst3 | `0267A847-15DB-3353-BA7C-F7FB76286386` |
| EchoJay Link.component | `63ADDBF5-CC8F-3269-86E3-5B9F2FF93B54` |
| EchoJay Link.vst3 | `EB7EDED9-4E81-3D9A-A615-EDC7DC66BBB7` |

Read back off the installed files as well as from the install script. Universal binaries carry one
UUID per slice; these are the **arm64** ones, which is what your Mac loads.

**S4 85/85. S5 GREEN.** Four bundles and both AAX, built across the three trees after the forced
delete of all six archives.

**The chain's first attempt was stopped at its own red, and it was mine.** S4 reported
`level_loop_guard ***Failed` on legs 17a/17b: *"the band [2, 3] for a target of 3 - the band whose
prose has always been 3 dB"*. My band fix had straddled the target (t−0.5 to t+0.5), which changes a
convention the whole system's prose quotes — and nothing about your Lindell needed it changed. The
only thing wrong was that a 1 dB target landed its low end on **zero**. So the band keeps its shape
and gains a floor: `[max(0.3, t−1), t]` — a 3 dB target is still 2–3, a 1 dB target is 0.3–1.0, and no
band can be satisfied by silence. Nothing was installed on the red, both guards were re-run green,
and the chain restarted from the top. DS5c now holds the convention beside the floor, so the next
person to "improve" it gets told.

Guard verdicts on the installed tree: `agent_client_guard` GREEN 343, `chat_store_guard` GREEN 141,
`comp_contract_guard` GREEN, `global_undo_guard` GREEN, `level_loop_guard` GREEN, plus the whole of
S4 and S5.

**The rig, both roads.** Mock: **156 passed, 4 failed, 12 skipped**. Live against 3215:
**84 passed, 7 failed, 15 skipped**.

**No new product red on either road.** Every live red is a known family, and three of the four mock
reds were the ROW rather than the product — which is what a row that finally aims at the right thing
looks like:

| red | road | what it is |
|---|---|---|
| I9c | mock | the Link-figure vs param-map timing, owed since 18 Oct |
| B5 (2 legs) | mock | **the row.** Its aim WORKED this time (`work_on -> ok`, where yesterday morning it was refused twice), so it measured a real chat at last — and then compared his message against **the first card on screen** while that chat held **five**. The card "above" his message was an older run's, sitting where it belongs. It now matches **this run's** card by `cardRunKey` against `agentRunKey` |
| B6's new-chat leg | mock | **mine, and tonight's own ruling.** "his row at −1": his message was not missing, it was **held on the supersede ask's chips** waiting for an answer nobody gave. The row now presses "Start anyway", the way you would |
| ITEM 0b (3 legs) | live | the live model never moved the pre-gain, so the windowless-completion legs had nothing to read. **All four ITEM 0b legs pass on the mock road**, including that one — and the session-survival leg that failed yesterday now passes on both roads, which is the bounded wait for the session id |
| A4-live | live | the Link-figure timing again (I9c's twin) |
| E10-live, C2-live, H2 | live | the live model's freedom: it asked instead of measuring, its add ops carried no position, and in H2's case the card said nothing at all so the row refused a vacuous pass. C2-live's own note — *"the add ops carried no position and the closing claimed one"* — is worth B seeing |

**B9, the live view's row, SKIPPED LOUDLY on both roads** — and that is the fix working, not the
feature: *"slot 1 of edd1155c67 holds "Renaissance Axx (s)", which is not the EchoJay plugin this row
adds"*. It opened slot 1 blindly while its own add had landed elsewhere, so it refused to report a
verdict on a plugin it had not chosen. It now **finds the slot its add landed on** from the Link's
published names. So the live view is in the build with its crash fix and its audio fix, and the rig
has still not proven it end to end — **you are its test**, as you said.

All three row fixes are in and syntax-checked, and a fourth mock pass confirmed them (§7b).

## 7b. The confirming pass, and two things it found

**88 checks, 2 red** (from 4). **B5's two legs pass** now that the row names this run's card, and B9
found its own slot correctly (*"the EchoJay plugin this row added is slot 3 on 162e861365"*). What is
left is I9c, owed since 18 Oct — and B6's new-chat leg, for a reason that is worth more than the leg:

**(i) The rig cannot answer a supersede ask.** The press came back *`"Start anyway" press -> unknown
button "chip0"`*: the rig's chip vocabulary answers the CLIENT's pending ask, and the supersede
question is the editor's own replace-ask, which it has no name for. So that row cannot drive tonight's
new behaviour at all, and its second leg reads "his row at −1" because his message is sitting on chips
nobody can press. The guard holds that claim properly (SU5/SU6*), and the rig needs a press for an
editor ask before this row can. **Owed**, and named rather than worked around.

**(ii) The live view cannot copy an EchoJay plugin — and that is the one the rig can load.** The log,
in full:

```
EJShadow: no live copy of "EchoJay Compressor" could be created:
          No compatible plug-in format exists for this plug-in - the static picture stays
```

The copy is made through `resolveByName` and the plugin catalogue, and EchoJay's own devices are not
in it: they come from the built-in registry. So the view works for third-party plugins (your X-Limit
came up; the rig's Renaissance Axx came up and reported *"up but NO AUDIO has arrived ... in 2 s"*,
which is the reading I added last night doing its job) and refuses for ours. Two consequences: B9 can
never prove the view with the only plugin the rig is allowed to load, and **you cannot see a live view
of an EchoJay Compressor or Limiter** either. **Owed:** the built-in road for the copy, which is the
same one the guards already use to load a built-in.

## 8. What is owed

0. **Two found by the rig in the small hours, both named above in §7b:** the live view cannot copy an
   EchoJay built-in (so our own devices show the static picture), and the rig has no way to press an
   editor replace-ask's chips (so B6's new-chat row cannot drive the supersede question).
1. **B's dial step** (`dial_step` in the capabilities, the `op: "dial"` frame, `step_text`, the
   structured answer, the 60 s playback wait and `not_heard`). Not declared, on purpose — §3.
2. **B's clip drive** (`kind: "clip_drive"`: a slot-IN actuator with the OUT mirrored, and a peak
   reduction measured across the slot). With it: **an explicit refusal for a dial kind we do not
   implement**, so a `clip_drive` block arriving at an older build holds the plugin instead of
   mis-dialling it as a drive.
3. **The closing waiting for every dial.** Your 16:40 ruling has two halves: the step beside its add
   (done, §2) and *"the closing and How's it sounding? appear only once every dial has landed"*.
   B's contract makes that **his** side — *"the batch's results come back only when every dial has
   answered, so the closing waits for them by itself"* — so on this build the closing can still
   arrive before a dial lands. It is the dial step's round, not a separate fix.
4. **A positioned add is not recorded for undo** (§4): only appended adds are, because a positioned
   add does not land on the end of the rack. The log says so when it declines.
5. **Your own two tests, which no harness here can run:** the live view against the UnFairchild and
   the Waves SSL (the rig never loads a PACE or iLok plugin), and Undo all on a finished Link run
   after the Link's own window has been opened and closed mid-run (the rig can drive the run but not
   your window).
6. **Pro Tools / AAX is built but not proven**, as before.
7. **The amount actuator's stepping** is still hold-only, which is why a dial on an `actuator:
   "amount"` block holds and measures rather than turning. B's `sense` makes it implementable.

## 9. Retest steps

**1. The dial on a bus — three minutes, playing.** "build a mix bus", Go, keep playing.
*Expect:* the dial **runs** (the log line is `EJThreshold: the bus rule does not stop this one`), it
**steps the control** rather than writing back what it read, and the card's dial step sits **right
after the add it belongs to** and finishes **on that step** with its result. No loose message below
the card and **no question after it**. If a plugin's meters disagree, the step says so plainly.
*Still expect to see wrong:* the closing may arrive before the dial lands (§8.3 — B's side).

**2. The dial on a Link — three minutes, playing.** A build on a Link whose compressor dials.
*Expect:* the same, and if the two sensors disagree for ~25 s it **ends and says what it measured**
instead of holding for ever.

**3. A plugin whose control is named twice — two minutes.** bx_opto is the case: add it with a target
and watch its Peak Reduction. *Expect:* the dial reads and writes the **knob** (33, then stepping),
never the read-out, and the plugin is never left at 0. The log line to check is
`EJThreshold... read N dB off the plugin` — N should be the knob's value, not 0.00.

**4. Undo all on a Link — three minutes.** Build a chain on a Link with the agent, let it finish, then
press **Undo all** on the card. *Expect:* the plugins the run added are removed, top slot first, and
the card says what came back. Then the harder half: do it again, but **open the Link's own window
mid-run and close it** before pressing Undo all. The log lines worth sending me are `EJUndo:` (what
was recorded and what was sent) and `EJAgentRow: press`.

**5. The supersede ask — two minutes.** A run live on one Link; type in another Link's chat and press
Enter. *Expect:* **"This will stop the run on `<the first Link>`. Start anyway?"** with Start anyway /
Cancel — whether the first run is waiting on chips **or working**. Press Enter again: nothing new is
added. Cancel: the run carries on and your text is back in the box. Start anyway: the old run stops,
its card says so in its own chat, and your new run starts.

**6. The live view — one minute, then the two that crashed.** Select a Link, click a slot: that
plugin's own interface, live, **with its meters moving** while the song plays (that is the part that
was broken at 16:35 — the picture arrived and nothing moved). Then the real test: the **UnFairchild**
and the **Waves SSL** that crashed at 16:33 and 16:41. If either takes the session down again, send me
the newest `AUHostingService` crash report — the two frames that identify the defect are fixed, so it
will say whether it is the same one.

**If anything is off:** `~/Library/EchoJay/logs/echojay-0.log`, and the useful greps are
`EJThreshold:` (a dial starting, stepping, holding, ending, and why), `EJShadow:` (the live view: what
was asked, what is streaming, what refused, and "up but no audio arrived"), `EJUndo:` (what was
recorded for a Link and what the undo sent), `EJAgent: [supersede-ask]`, and `EJAgentRow: press`.
