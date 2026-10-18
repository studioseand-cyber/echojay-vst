# Morning report — 18 Oct 2026

*(the overnight run of 17 Oct: the §25 fix, the RP8K pair, the full gate, Kathy's recheck pack)*

## 1. The gate: RED, and neither red is the build

Gate 27 ran 22:58:54 to 00:59:56 on `a645cde0`, Logic closed, nothing installed by it.

| stage | verdict | |
|---|---|---|
| S1 | green | forced delete of six archives, relink across three trees |
| S2, S3, S3b | green | guard targets, the four bundles, the archive-provenance check |
| S4 | green | **79 of 79** guard tests |
| S5 | green | the two-sided link guard |
| S6 live half | **RED** | 3 failed, 65 passed, 6 skipped |
| S6 mock half | **RED** | 1 failed, 117 passed, 13 skipped |

**The three live reds are B's server, and the plugin is provably not in them.** A3-live, A4-live and
C3-live all failed because the server refused Renaissance Axx *as a pick* — "its control map isn't
fully verified yet" — so **no `do` was ever issued**. The round posted two `look`s and a rack read
and nothing else, and the plugin marked **zero** plugins as unable to load, so this is not my
won't-load list leaking into B's A8 gate. Written up for B with the round's log as `FOR_B` §27.
In LIVE mode your server's own map store decides, not the map the rig samples, which is why every
row that adds a third party with a setting is unrunnable against 3213 tonight.

**The one mock red was the row addressing a Link that no longer exists — and it is still red, now
for a reason worth having.** I9c reads the *Link's* mirrored card against the Link's own ledger, and
the uid it addresses is captured much earlier in the pass; by the time the row runs, that Link is
gone. Nothing landed, so the row reported "(no slot carried both facts)" and read as a product
failure. I9b — same build, same plugin, same figure, on the own rack — passed in every run tonight,
and that is what clears the product of this red.

I ran it twice, because Kathy's pack finished at 01:30 rather than 07:00, which freed the machine
with no measurements left to disturb. Both runs are worth reading, because **my first fix was
wrong**:

**Run 5 (01:30, `work_on linkUid` before the ask): I9c failed again, in exactly the same words.**
The rig had already answered the question in a line I had not read:

    EJRig: work_on seq=3024 -> FAILED (that uid is not live)

`linkUid` is captured earlier in the pass and the Link behind it is gone by the time this row runs.
The row's guard tested that the *string* was non-empty, which is not the same question as whether
the Link exists — and the command that would have told it so had its ack thrown away. So the row
now resolves a Link this run can prove is alive, sends `work_on`, **reads the ack**, and treats a
refusal as a loud skip carrying the rig's own words.

**Run 6 (01:48): it aimed correctly — `work_on seq=3018 -> ok (working on 61cc4c030f)` — and I9c
still failed, now on something real:**

    EJLink: chain-cmd v2 -> 61cc4c030f seq=1792285348 source="agent" lease=none
    EJDialSummary: slot 1 ("Renaissance Axx (s)") … requested=1 applied=1 status=applied
    EJAgent: step tc_tp FAILED code=not_applied
             "no chain-ack-61cc4c030f.json with seq 1792285348 appeared in 8000 ms"

The add was sent to that Link and **no ack came back inside the 8-second window**, so nothing ever
landed for the mirrored card to carry. That is not the card's logic and not the row's target any
more: it is the chain-command ack path, which is queue item 16 (the 8 s window) sitting next to
item 0b (the rig hosts the INSTALLED Link in a fresh process) and item 13 (a Link looking gone for
a few seconds after its rack is rebuilt). The log also shows the editor renewing a lease for a
*different* uid (`da53af5fa3`) while the command went to `61cc4c030f`, which is the first thing I
would pull on tomorrow.

I stopped there rather than keep going: you said item 13 is first tomorrow and needs rig time, and
this finding is in its family. The row fix stands and is strictly better — it no longer measures a
dead Link, and it surfaced this instead of hiding it — and it is **built and run** now, not
unverified. I9b passed in both runs, on this build, which is still what clears the product of the
original red.

**No QUICK build tonight, and nothing new installed**: no product change is implicated by either
red, so there was nothing to rebuild. What is in `~/Library` is still the binary this gate verified.

## 2. RP8K, red then green

End to end, in the rig, with your 1,013 plugin names in the catalogue and your 357 won't-load
entries in the folder the product actually reads. Same fixture, same seeds, two builds:

| pass | the build it hosted | what the plugin POSTED |
|---|---|---|
| **RED** | `ship_2026-10-17b/EchoJay V2.component` — 17b's own bundle, no rebuild needed | **37,122 bytes**, keys `[… notLoadable, notLoadableNote, … truncated]`, **no** `notLoadableTotal` |
| **GREEN** | this tree | **1,125 bytes**, keys `[… notLoadableTotal …]`, total 702, returned 60, notLoadableTotal 336 |

`37,122 → 1,125 bytes` on the same seeded input, and the whole green pass was clean:
`host_rig: GREEN (0 failed, 53 passed, 10 skipped)` against the red pass's
`RED (1 failed, 52 passed, 12 skipped)` — one failure, and it was this row.

The red half is 17b's code, not a reconstruction: the rig registers the AU from the given bundle's
own factory function, so the installed build is never consulted. Note `truncated` in those keys —
17b's gate *did* run and *did* trim, and the result still went out at 37 KB, because the full
won't-load list was appended afterwards. That is §25 in one line.

**Why the inventory reads 702 rather than 1,013**, since a number that does not add up is worth
explaining before you ask: the catalogue holds all 1,013, the product then withholds the 336
distinct names it knows cannot load here (the 357 entries are 336 names across two formats) and adds
its own built-ins — 1,013 − 336 + 25 = 702. Both halves are seeded identically, so the comparison is
like for like.

### The pair that already stands, at the layer the bug lived

| | the input | what `ExecutorRead::inventoryPage` produced |
|---|---|---|
| **SZ3 (17b's shape)** | your 1,013 installed names, 357 unable to run here | **49,186 bytes** — over the contract's 8 KB |
| **SZ2 (this build)** | the same input | **2,702 bytes**, total 1013, returned 60, notLoadableTotal 357 |

SZ3 fails if 17b's shape ever fits, and SZ2a fails if the snapshot is missing, so neither leg can
pass on nothing. `agent_client_guard` GREEN, 127 ok.

## 3. What is installed

    EchoJay V2.component    9CD06900-DC9F-3A6D-A8DA-7D44C3079E8A   md5 69e4e1ef53e0a4e9dfd3ab1983650c2e   57,070,408 bytes
    EchoJay Link.component  3DA1BAB8-0B7E-3031-A6D2-1636EB177674   md5 d08d021f2932eb323106db2babccf90e   33,173,968 bytes
    EchoJay V2.vst3         A15250EF-3FCD-3CCD-9711-871CD1B0168F   md5 79e3c0a1f3dd6ac023d96969378ff589   57,164,600 bytes
    EchoJay Link.vst3       B3A9F0DF-2A31-32B6-BC1B-13DA3370925C   md5 9dda106a1f210268ec360dd843c65265   33,253,632 bytes

The V2 carries the §25 fix; both Link binaries are 17b's unchanged, because the Link target compiles
none of the files this round touched. Nothing was installed after 22:13 — see the disclosure in
`docs/REPORT_2026-10-17c_QUICK.md`.

## 4. Kathy's recheck pack: started 01:03:14, preflight passed

Unzipped to `~/Downloads/SEAN_RECHECK_PACK_16oct`, quarantine cleared, then
`bash ~/Downloads/SEAN_RECHECK_PACK_16oct/recheck.sh`. Its own preflight passed on every line:

    build ad50ddbb; app signed team 8BT5F9B887; probe beside the executable,
    cdhash 7adf508f38…; iLok present (0x00120000 / 1); power AC; sleep timer off;
    quarantine: none on the app, its executable or the probe

Then: 46 units from `held_units.txt`, 5 steps to run and 27 done or skipped, ETA 2:10, stops at
07:00, state in `~/Library/ejmap/cert/run_all.json`, log in `~/Library/ejmap/cert/run_all/` and
`recheck_16oct.log`. It also found **86 tone checks made by a build other than `ad50ddbb`**, so the
follow-up re-runs those stale checks first — that is the step it spent the first hour in.

### Two of its steps refused instantly, and that is in Kathy's pack, not your Mac

`accuracy_gaps_drafts` and `accuracy_gaps` both exited 2 within a second of starting, with the same
line in `~/Library/ejmap/cert/run_all/accuracy_gaps*.log`:

    --redo: 'accuracy_gaps' is not a Phase B category, nothing_nominated, uad or no_pool

`recheck.sh` passes `--redo accuracy_gaps`, and this build (`ad50ddbb`) does not accept that value —
it wants a Phase B category, or `nothing_nominated`, `uad` or `no_pool`. So the script and the app
in the same zip disagree about one flag. Nothing was measured by those two steps and nothing was
corrupted by them; part 1 ended `exit 1` with its other three steps done.

I did not rewrite Kathy's flag. Which category those two steps were meant to redo changes what gets
certified, and guessing it would put wrong numbers in your cert folder — that is Kathy's call, and
it is one line in `recheck.sh` once she says which value she meant.

**It finished the whole sequence at 01:30, in 28 minutes rather than the night**:
`RUN-ALL: 2 done, 0 failed; the sequence is complete (cert/run_all.json: every step done; delete it
to run the whole sequence again)`. Most of its plan was already done or skipped from earlier runs,
the follow-up re-ran the 86 stale tone checks in 15 minutes, and `multiband_v1` took 11 against its
own hour-long estimate. What it did NOT do is the two refused steps below, and 40 tone checks from
another build that part 2's plan deliberately leaves stale.

Part 2 was the hour that mattered: `multiband_v1` (per-band tone checks,
crossovers, make-up across 17 multiband records), then `drafts`. Its plan also notes **40 tone checks
made by another build stay stale**, because the follow-up step is not in part 2's `--steps`.

### One disclosure about the measurements

At about 01:18 I ran `ejmap --help` to find out which `--redo` values the build accepts. It did not
return — it ran for roughly two minutes before I killed it at 01:20, alongside the measuring. So any
unit measured in that window (the first minutes of `multiband_v1`, around C6 (m) and
C6-SideChain (s) in `multiband_v1.log`) had an extra process on the machine and should be
re-measured before it is trusted. The measuring run itself was never interrupted, and nothing else
of mine has touched the machine since 01:03. Asking a measuring app a question during its own
measurement window was my error.

**It started 4 minutes after the gate ended**, and the only reason it was not 4 minutes after
midnight is that the gate's own S6 takes an hour. From 01:03 I ran nothing that loads a plugin and
nothing that builds: the I9c fix above was written and syntax-checked before the pack started.

## 5. Your Logic recheck, on B's server 3215 (about 9 minutes)

First, point the plugin at 3215 and prove it:

    echo "http://127.0.0.1:3215" > ~/Library/EchoJay/dev_base_url.txt

**then start Logic** (the override is read once per process, so a file written while Logic is up does
nothing). **PASS:** the editor's bottom-right marker reads `127.0.0.1:3215`. Blank means production,
and then nothing below is testing B's tree. To go back: `rm ~/Library/EchoJay/dev_base_url.txt`.

Agent mode **Ask**, the project with the Mix Bus and the Kick's Link.

1. **The inventory, the one that killed every run** *(new this round)*. On the Kick's Link, ask
   something that makes it look at what you own — "what compressors do I have?". **PASS:** it answers
   and the run carries on. It must name the scale rather than list it ("1,013 plugins, 357 of them
   can't run here"), and no run stops with a tool id or a contract reference in the line.
2. **Retry on a stuck chat** *(new this round)*. If any run ever does stop, press **Retry**, then
   send a new message in the same chat. **PASS:** neither repeats the same failure — the old result
   is never posted again. This is the one from your screenshot where both failed with `toolu_01VtSX…`.
3. **A stop in plain words** *(new this round)*. **PASS:** whatever a stop says, it reads as a
   sentence about what to do next — no `toolu_`, no `CONTRACT_AGENT_TOOLS`, no byte limits.
4. **New chat** (item 1). On the Kick's Link, ask for something that ends in a question. While it
   waits, press **+ New chat**, aim it at the Mix Bus, type anything. **PASS:** the old run is gone,
   the answer is about the Mix Bus, nothing from the Kick run is drawn in the new chat.
5. **The tap** (item 2). Ask for a compressor on the Kick's Link and **tap a chip**. **PASS:** it is
   added with no Apply press.
6. **A plugin that failed before** (items 3 and 10, your ruling). "Add kHs Transient Shaper" on the
   Kick's Link. **PASS:** it says up front that it won't load on this Mac, offers **[Try anyway]**
   plus working alternatives as chips, and the line carries no OS error text (that is under Details).
   Tapping **Try anyway** must really try it. B's 20:30 fix means this must also hold when you or it
   says "Pro-C 2" and the mark says "FabFilter Pro-C 2".
7. **"Added" before the ack** (item 4). Target the Kick's Link, ask for Renaissance Axx with a
   threshold. **PASS:** nothing says "Added …" until after Apply, and no line names the Mix Bus.
   **Still not reproduced in the rig — your eyes are the instrument.**
8. **A typed name** (item 5). When it asks which plugin, type **cl1b**. **PASS:** it answers the
   question and the run carries on. Then type a sentence ("set the pre-gain to 2 dB") into a waiting
   run: **PASS:** that stops the run and starts its own.
9. **The editor's tab and the mirror** (items 7 and 8). From the **Chat** tab, select a rack slot:
   **PASS:** the window switches to Chain and the editor draws there, never over the chat. Then select
   the CL 1B slot in the main window's mirror: **PASS:** the whole "running on 88_7even Kick_2_01…"
   sentence is readable and **Bring it forward** sits under it.
10. **The chat list** (items 11 and 12). Press New chat two or three times without typing: **PASS:**
    no empty entries accumulate. Then, in a chat with a few turns, aim the composer at another track:
    **PASS:** you land in that track's own chat.

Steps 1–3 are this round's; 4–10 are 17b's list, unchanged except step 6, which now also covers the
vendor-spelling fix B deployed at 21:37. Step 2 of the old list (the pre-gain wording) is covered by
B's `set_pre_gain` work and is worth a glance if you ask for a change in dB.
