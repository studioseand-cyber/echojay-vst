# THE NIGHTS FROM 10 Oct (Kathy's priority) — night 1 on 00f2ae79, then the next build; one line per night

Build and install **00f2ae79** as the next section says (same folder, same `cert/run_all.json`, nothing deleted; the 9 Oct
review-picks copy still comes first if you have not done it). Then ONE line a night, in this order. Each stops at 07:00 by itself.

```
# Night 1 (tonight, on 00f2ae79)  ~5.1 h   the re-checks, the fix-ups, EQ (resumes), saturation
caffeinate -i "$BIN" --run-all --steps preflight,followup,fixups,eq,saturation,drafts --until 07:00
```

From night 2 on, run the NEXT build (cut after Kathy has checked night 1's zip; install it over 00f2ae79 the same way - same folder,
same `cert/run_all.json`, nothing deleted). Its first night re-checks every tone check once (they were made by 00f2ae79: the
re-check rule, ~40 min, in night 2's hours). The next build measures Phase B rows in parallel: **start at `--jobs 2`; go to
`--jobs 4` only after Kathy has checked a night at 2.**

```
# Night 2  ~6.5 h at --jobs 2 (~4.8 h at 4)   the follow-up once, reverb / delay, transients / gates, limiters as compressors
caffeinate -i "$BIN" --run-all --jobs 2 --steps preflight,followup,fixups,reverb_delay,transient_gate,limiter_comp,drafts --until 07:00
# Night 3  ~6.6 h at --jobs 2 (~5.9 h at 4)   no_pool (resumes), categorise, the accuracy passes, tuners
caffeinate -i "$BIN" --run-all --jobs 2 --steps preflight,followup,fixups,no_pool,categorise,combined,material,frequency,samplerate,tuners,drafts --until 07:00
# Night 4 (and 5)  ~14 h in all, the same at any --jobs   the strips (they run one at a time: see below)
caffeinate -i "$BIN" --run-all --jobs 2 --steps preflight,followup,fixups,strips,drafts --until 07:00
```

| night | steps | serial (--jobs 1) | --jobs 2 | --jobs 4 |
|---|---|---|---|---|
| 1 (tonight, 00f2ae79) | re-checks, fix-ups, EQ, saturation | 5.1 h | - | - |
| 2 | follow-up, reverb / delay, transients / gates, limiter_comp | 10.0 h | 6.5 h | 4.8 h |
| 3 | no_pool, categorise, combined, material, frequency, samplerate, tuners | 8.1 h | 6.6 h | 5.9 h |
| 4 (+5) | strips | 14.0 h | 14.0 h | 14.0 h |
| nights left incl. tonight | | ~6-7 | ~5 | ~5 |

How the parallel run works, for the morning output:
- Within a step, up to N products measure at once; each in its own folder, the row written only when it is complete.
- Some products always run ALONE (the pool empties first): every UAD unit, everything your licences.csv governs or a demo, every
  PACE / iLok product (and one whose PACE state cannot be checked), and anything that has ever shown a window, a licence stop, a
  timeout, refused to load, gone silent or crashed. The log lists them as "serial lane: <category>: <product> (why)". The strips
  run alone too (bx_console N gave different readings run to run even one at a time, so parallel could not be proven safe).
- No new product starts while the Mac's free memory is under 1.5 GB; at 07:00 / Ctrl-C nothing new starts and the products
  that were running are thrown away and run again next night - nothing finished is ever lost.
- The ETA line still prints the serial hours; at --jobs 2 expect the table's.
- Why nights 4-5 do not shrink: the strips (14 h) are serial. --jobs 4 saves time on nights 2-3 but not a night overall.

The rules for these lines:
- **Only move to the next night's line when the morning's output says the line's sequence is complete.** If 07:00 (or Ctrl-C)
  stopped it, run the SAME line again the next night: it resumes, and nothing finished is run again. (Moving on early would let
  the next line's steps go first: the run always takes steps in its own fixed order, not the order typed.)
- **preflight and drafts are in every line on purpose**: with `--steps`, only the steps named run, so they must be named. The
  preflight checks the app and probe every night; the drafts are re-derived every night from the records on disk.
- **followup and fixups are in every line too, and cost nothing once done**: the follow-up runs again only if some tone check was
  made by another build (night 1: 86 on your folder), the fixups once. If a night's output starts with
  "RUN-ALL: N tone check(s) made by another build", the follow-up ran first that night - that is the rule, not a fault.
- **limiter_comp, --jobs and the strips-serial rule exist only in the next build**: night 2 onward need it installed.
- **The UAD-2 Satellite (next build)**: if it is not seen, the UAD rows are LEFT UNRUN (not filed) and the step says "waiting for
  the UAD-2 device" - it resumes by itself the next night the Satellite is connected and powered; the 44 UAD EQs filed "device not
  connected" on 10 Oct run again then. Check `--uad-preflight` says PRESENT before a night if you can.
- `nothing_nominated` is in no line: every row it would re-run is in a category these lines re-run whole.
- The hours are the dry-run's ETA on a copy of your cert 2 + cert 3 (plan only, nothing loaded); nights 2-6 were dry-run with the
  next build's sources on a copy with the earlier nights marked finished, so each shows that night's own plan. limiter_comp's hours
  are measured here: 467 s per limiter (3 rehearsed) x ~43 of your 46 that load.

Every morning, as before: `--zip ... --since marker`, then `--cert-review-zip` on that zip.

---

# 10 Oct (Kathy's rulings) — the switch to 00f2ae79 (install as here; the night lines in the section above replace this line)

Build **00f2ae79** from the commit on your Mac as before (packaged and signed here at ~/Desktop/ejmap-dist-10oct-b; 5ab8352b is its
fallback). Install it over 5ab8352b - same folder, same `cert/run_all.json`, nothing deleted. The line this section first gave (now replaced by the night lines above):

```
caffeinate -i "$BIN" --run-all --skip nothing_nominated --until 07:00
```

What it does differently from last night (rehearsed on a copy of your cert 2 + cert 3, 10 Oct):
- **The stale tone checks go first.** Every tone check made by an older build is re-checked under 00f2ae79 - the follow-up step
  runs again although it is marked done (85 checks on your folder: the 38 from 5 Oct, the ones the 9-10 Oct fixes change - VBC
  FG-Red / Grey / MU / Rack, AMEK, the linked pairs, the meters - and, by the rule, the rest once). MAGNUM-K is re-swept with its
  pair fix. A check made by 00f2ae79 is never re-run by 00f2ae79.
- **The fix-up items are folded in:** right after no_pool resumes, a `fixups` step re-runs every multiband row (the 9 Oct fixes)
  and the gain-all rows that timed out or failed (PrimalTap, LISA, Auto-Tune Vocal EQ, Vocal Reverb, EchoBoy resume from their
  traces; bx_rooMS reads its labels in chunks; 2C-Aether is read again; AVOX SYBIL is filed unhostable). No separate fix-up night.
- Everything else you finished stays finished; EQ and no_pool resume where they stopped.
- Order: preflight, the follow-up (stale checks), no_pool (resumes), fixups, categorise, combined, material, frequency, samplerate,
  tuners, EQ (resumes), saturation, reverb / delay, transients / gates, strips, drafts. About 1.5 h more than last night's plan for
  the re-checks and the fix-ups; still about four nights in all.

The 9 Oct section below (the review picks copy, the line) still holds; this replaces only the build.

---

# TONIGHT, 9 Oct (Kathy's ruling) — 5ab8352b, the run that is still owed

You are on **5ab8352b** (built from the commit on your Mac; your preflight passed). Your `cert/run_all.json` already has preflight,
multiband, limiter, de-esser and gain-all done and EQ started; none of that is run again. What is still owed is everything the
compressor follow-up and Night 1 below would have done, then the rest of the plan. In this order:

```
# 1. FIRST: the 7 Oct review picks (it adds bx_console N and bx_console SSL 4000 E, which the strips' compressor sections need)
cp ~/Desktop/review_picks.json ~/Library/ejmap/cert/review_picks.json
# 2. then ONE command, every night until it says the sequence is complete
caffeinate -i "$BIN" --run-all --skip nothing_nominated --until 07:00
```

It runs, in this order: preflight, the licence check and stamp, the Satellite check, **the compressor follow-up** (the 69 exported
profiles are re-checked and re-exported on it), gain / timing, the uad / no_pool / categorise / combined / material / frequency /
samplerate / tuners sets, **EQ resumes** (the 126 finished EQs are kept), saturation, reverb / delay, transients / gates, the
strips, the drafts. `nothing_nominated` is skipped on purpose: every row it would re-run is in a category this run re-runs whole.
At your Mac's measured pace: tonight ~10.5 h (everything up to EQ's last 49, then saturation starts), night 2 saturation, reverb /
delay, transients / gates and the strips start, nights 3 and 4 the strips, then the drafts - about four nights. 07:00 or Ctrl-C
stops it; the same line carries on. The morning: `--zip ... --since marker`, then `--cert-review-zip` on that zip (below).

Everything after this section is the earlier plan and its history; follow this section tonight.

---

## AFTER THE MAIN RUN ENDS: the fix-up night — c7f762ed (Kathy's ruling, 9 Oct)

Only when tonight's `--run-all` line above says the sequence is complete. Install the fix-up build (c7f762ed, packaged and
signed at ~/Desktop/ejmap-dist-9oct-b; derive-only EQUAL to dc77d0a5 on your current folder, your 4 Oct zip, cert_sc and cert_tc35; 5ab8352b is its
fallback) the same way as before - same folder, nothing deleted - then ONE command:

```
caffeinate -i "$BIN" --phaseb-all --redo multiband,unfinished --category multiband --category gainall
```

It re-runs ONLY:
- every multiband row (17 on your Mac): only each band's own compressor threshold (never a limiter, gate or processor stage's),
  bands measured where a unit has no crossover control (OTT, Ozone 12 Dynamics, DynOne3), each Pro-MB band's own Low / High
  crossover range, sidechain filters never bands, and a band that is off at instantiate switched on by its own depth / level /
  range (DynOne3's Volume at -Inf, Pro-MB's Range at 0 dB, C6's floating bands' Range at 0) when no switch does it - and that
  write is on the record and the draft as a neutral write the server makes too;
- the gain-all rows that timed out or failed (8 on your Mac): PrimalTap, TOMO Audiolabs LISA, Auto-Tune Vocal EQ, Vocal Reverb
  and EchoBoy resume from the traces they already have, with a guard sized to their plan; bx_rooMS reads its labels in chunks;
  2C-Aether is read again; AVOX SYBIL is filed `unhostable` with the OS's error (it refuses to initialise) and not tried again.

Nothing else is touched: every other gain-all row (507 ok, 81 needs_licence, 2 windows) stays as it is, and no product without a
gain-all row is started. About 1.5-2 h (multiband ~1 h at your Mac's pace, the gain-all eight ~30-60 min). Then the drafts:

```
"$BIN" --phaseb-drafts
```

and the morning zip and review as usual.

---

# Running the 7 Oct build (dc77d0a5) — Sean's Mac, follow as written

`ejmap.app` built from commit **dc77d0a5** (branch feat/ejmap-cert; packaged and signed at ~/Desktop/ejmap-dist-7oct-b per
docs/PACKAGING_EJMAP_APP.md; 0f175c51 the fallback). Everything below is complete and in order; nothing refers to a later section. Every run resumes
from where it stopped when started again; Ctrl-C any time. Keep the lid open (`caffeinate -i` keeps the Mac awake).

```
BIN=/Applications/ejmap.app/Contents/MacOS/ejmap
"$BIN" --cert-preflight            # must say: app signed, probe beside the executable, cert root ~/Library/ejmap/cert
```

## Step 0 — the licence file, the Satellite, Accessibility (nothing loads in this step)

```
# 0a. copy your licence file into the cert folder (vendor,product,state,demo_end,note; other vendors go in the same file)
cp ~/Desktop/licence_states_UAD_2026-10-06.csv ~/Library/ejmap/cert/licences.csv
# 0b. the alias table and the review sheet -> ~/Library/ejmap/cert/licence_review.txt
"$BIN" --licence-check ~/Library/ejmap/cert
# 0c. last night's rows: demo rows and their records stamped, expired / unowned / unmatched rows filed needs_licence
"$BIN" --licence-stamp ~/Library/ejmap/cert
# 0d. the Satellite, connected and powered: must print PRESENT
"$BIN" --uad-preflight
```

The file governs ONLY plugins of vendors that appear in it (UAD-2 today): a UAD plugin the table cannot match is on
`licence_review.txt` and is not loaded until you add a line or a "covers ..." note; a plugin of a vendor NOT in the file
(Waves, Plugin Alliance, everything else) runs exactly as before. If 0d prints ABSENT with the Satellite on, add
`--assume-uad-device` to every command below and send me the lines it printed. For the TEXT of a licence window to be
recorded (titles always are), allow `ejmap` once under System Settings › Privacy & Security › Accessibility.

## Night 1 (about 8 hours on your Mac; in this order, each command its own go)

```
# 1. the compressor follow-up: ~35-40 min (47 checks + 2 re-sweeps; the 21 owned held UAD compressors unhold when the Satellite is seen)
caffeinate -i "$BIN" --cert-tonecheck-all 2>&1 | tee -a ~/Library/ejmap/cert/tonecheck.log
# 2. the gain and timing data for every certified compressor: ~50 min
caffeinate -i "$BIN" --phaseb-all --redo gain-cal,timing 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log
# 3. the redo sets
caffeinate -i "$BIN" --phaseb-all --redo uad 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log            # ~0-10 min: only UAD rows the stamp left and the Satellite admits
caffeinate -i "$BIN" --phaseb-all --redo no_pool 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log        # the 10 Soundtoys / 2C: ~30 min
caffeinate -i "$BIN" --phaseb-all --redo multiband 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log      # 17: ~1.5-2 h
caffeinate -i "$BIN" --categorise-propose --include-pace 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log   # the 605: ~1-1.5 h -> cert/proposed_categories.json + category_review.txt (never categories.json)
caffeinate -i "$BIN" --phaseb-all --redo combined 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log       # ~20-40 min (three reads per compressor at a scaled hold)
caffeinate -i "$BIN" --phaseb-all --redo material 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log       # ~70 min
caffeinate -i "$BIN" --phaseb-all --redo frequency 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log      # ~70 min
caffeinate -i "$BIN" --phaseb-all --redo samplerate 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log     # a spread of 10: ~8 min
caffeinate -i "$BIN" --phaseb-all --redo tuners 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log         # 6: ~30 min
```

## Night 2 (the channel strips: 8-14 hours; stop in the morning, the same command resumes the next night)

```
caffeinate -i "$BIN" --phaseb-all --redo strips 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log
```
Each strip: its sections by name, each through its own mode, the compressor section through the full sweep and tone check into
its own folder (`cert/phaseb/strips/strip/<stem>.compressor/`), nothing exported. 1-3 min a strip here, an EQ section with named
bands 6-12 min. To try a few first: `--only "bx_console SSL 4000 E" --only "Lindell 80 Channel"` on the same command.

## Night 3 (about 11.5 hours)

```
caffeinate -i "$BIN" --phaseb-all --redo gain-all 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log             # ~500 products: ~7 h
caffeinate -i "$BIN" --phaseb-all --redo nothing_nominated 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log    # 205 rows: ~4.5 h
```

## Stop, resume, status

Ctrl-C stops any command; the product mid-measurement leaves nothing behind. The same command resumes. At any time:
```
"$BIN" --phaseb-status
```

## The morning zip (stop the run first)

```
# the first morning: the whole cert folder
"$BIN" --zip ~/Library/ejmap/cert --out ~/Desktop/ejmap-$(hostname -s)-$(date +%Y%m%d).zip
# every later morning, once the folder has passed 500 MB (the first zip says so): only what is new since the last zip
"$BIN" --zip ~/Library/ejmap/cert --out ~/Desktop/ejmap-$(hostname -s)-$(date +%Y%m%d).zip --since marker
```
`config.json` is never in a zip (the command skips it). Projected ~700 MB gzipped for the three nights together.

## Tonight and the switch-over (Kathy, 8 Oct): a6c85e6f WITHOUT multiband, then 5ab8352b WITH it

**Tonight, after Night 1 above: a6c85e6f** (packaged and signed at ~/Desktop/ejmap-dist-8oct-d; derive-only EQUAL to dc77d0a5 on your
current folder, your 4 Oct zip, cert_sc and cert_tc35 - 0 differences in rows, records, profiles and controls, so the compressor path
above is untouched). Nights 2 and 3 above are NOT run on dc77d0a5: their strips and gain-all are in this line, and their
nothing_nominated rows are all re-run by its category redos. Install it the same way as the build above, then, every night until
Kathy says to switch:

```
caffeinate -i "$BIN" --run-all --steps preflight,limiter,deesser,eq,saturation,reverb_delay,transient_gate,gain_all,strips,drafts --until 07:00
```

**Multiband is NOT in tonight's line on purpose.** a6c85e6f's multiband step takes Melda's gate / processor thresholds for band
thresholds and C6's floating bands for crossovers; the fix is in the next build. Nothing else in the line uses that code.

**The switch-over, on a later night: 5ab8352b** (packaged and signed at ~/Desktop/ejmap-dist-8oct-e; derive-only EQUAL to dc77d0a5 on
the same four sets, build tree and packaged; a6c85e6f is its fallback). It adds the multiband fix (only each band's own compressor
threshold; floating bands), the v0.2 drafts (Sean's rulings: full_wet, EQ default_bands, the gain drive curve, tuner step 1) and
preflight / drafts on every run. Install it over a6c85e6f - same folder, same `cert/run_all.json`, nothing deleted - then the same
line WITH multiband, every night:

```
caffeinate -i "$BIN" --run-all --steps preflight,multiband,limiter,deesser,eq,saturation,reverb_delay,transient_gate,gain_all,strips,drafts --until 07:00
```

What the switch does (rehearsed 8 Oct on scratch copies of your folder, `tools/ejmap/cert-traces/2026-10-08-switch/`):
- every step a6c85e6f FINISHED stays finished and is not run again (its rows are kept as they are);
- a step a6c85e6f was stopped in (07:00 or Ctrl-C) RESUMES: its finished products are kept, only the missing ones run;
- multiband, new to the line, runs from the start: `--redo multiband` re-runs EVERY multiband row, including the 17 your Night 1
  `--redo multiband` wrote on dc77d0a5 with the faulty nomination - none of them is kept;
- preflight checks the new app and the drafts are re-derived (v0.2) from every record on disk, every night.

It runs those steps in that order, each step's output in `cert/run_all/<step>.log`, one progress line a minute (done / total, elapsed,
ETA, the hour it stops). At 07:00 it stops the step it is in (that step's finished products are kept) and starts nothing new; the SAME
line the next night carries on from there (a step it stopped resumes without re-deleting what it already redid). Ctrl-C works the
same way. `--dry-run` on the end prints what it would run and changes nothing. The steps always run in THIS order, whatever order
`--steps` lists them in: multiband ~20 min, limiters ~25 min, de-essers ~10 min, **gain-all ~7 h**, EQ ~2.5 h, saturation ~2 h, reverb /
delay ~3.2 h, transients / gates ~32 min, strips ~14 h, drafts ~1 s - about 30 hours in all (the dry-run's ETA 30:08). Gain-all runs
before saturation on purpose: saturation's level match uses the output control the gain-all draft measured; a product with no usable
output there falls back to a control named output / volume / level / trim / makeup that is checked to be level-only after the drive.

| night (21:00-07:00) | runs | hours |
|---|---|---|
| A | limiters, de-essers, gain-all, then EQ starts (stopped at 07:00, ~2.3 h in) - tonight's line has no multiband | ~10 |
| B | EQ finishes (~0.4 h), saturation, reverb / delay, transients / gates, then the strips start | ~10 |
| C | strips | ~10 |
| D | the last of the strips, then the drafts | ~0.2 |

On the switch-over night, multiband (~20 min, every row) runs first, before whatever step the line had reached.

**nothing_nominated is not in the line, on purpose.** Every row it would re-run (276 in your 5/6 Oct folder: EQ 101, saturation 116,
limiter 16, timing 34, transient 2, reverb 3, gaincal 2, de-esser 1, delay 1) is in a category this line re-runs whole, or in gain /
timing, which Night 1's `--redo gain-cal,timing` re-ran.

**Roles are found twice.** For reverbs, delays, transient shapers and gates, the test that finds which control is the mix, the time, the
attack, the threshold... runs twice. If the two runs pick different controls, that role is left unassigned and filed `not_repeatable`
(needs review, both picks in the record and the draft's notes) - MTransient's sustain does this here. That is why those two steps take
about 1.6 times as long as a single run would.

The drafts come last (~1 second, nothing loaded): every category's draft,
the tuners' included - your Night 1 `--redo tuners` on dc77d0a5 already records whether a held note keeps its vibrato, and the
draft carries it from there.

**Multiband - not yet seen to work.** The enable step (a band that is off at instantiate is switched on by its own switch, never a gate
stage's) is built and pinned, but it has NOT yet been seen to succeed on a real plugin: on this Mac SSL G3 needed no enable (its bands
are on at instantiate) and MDynamicsMB's Processor 2 still cut nothing after it; FabFilter Pro-MB and Ozone 12 Dynamics are not
installed here and DynOne3 needs its iLok. Your run is what proves it - in the morning review, a multiband row whose band was off and
now cuts is the first live success.

**The morning** (stop the run first, or let 07:00 stop it):

```
"$BIN" --zip ~/Library/ejmap/cert --out ~/Desktop/ejmap-$(hostname -s)-$(date +%Y%m%d).zip --since marker
"$BIN" --cert-review-zip ~/Desktop/ejmap-$(hostname -s)-$(date +%Y%m%d).zip       # ~1 min: every Phase B category, its drafts, what ran and what did not
```

THE ZIP GROWS about +320 MB for these steps (the role tests read 5 ms windows and run twice; the maps keep 1 ms), and ~+70 MB for the strips; hence
`--since marker`.

---

# History — do not follow: the follow-up as first written (2 Oct 2026; L per level, licence skip and CL 1B import 3 Oct; re-sweeps 4 Oct)

After the batch has run with the build from `36397676`, the deep points (4/5/6 dB, spec v1.7 §3) and
the amended pick and tone check (§6.4, §8) need a newer build. For most rows **nothing is re-swept**: the
traces the batch kept in `~/Library/ejmap/cert` already hold every level at every position, so this mode
re-derives each exported record from its traces, re-exports the profile with the deep points, and loads each
certified plugin only for the tone checks. **The rows whose plan changed under this build are re-swept by the
same command, which decides that itself** (below); you never name a product. It is resumable and uses the same window watch.

**History.** Tonight-of-6-Oct's build was `0d2ccd5d` (6 Oct, 16:12; branch `feat/ejmap-cert`; packaged by `docs/PACKAGING_EJMAP_APP.md`
step 1 with that commit checked out). It is `9d3a1972` plus your Vocal Compressor ruling (an unmeasured detector exported as
`detector_f: null, detector_f_source: "unknown"` with the reason in notes; such a profile tone-checked at both L_ref values; the
detector retried at the −27 dBFS position). `9d3a1972` is the fallback: it runs everything else identically (checked equal,
derive-only, on your current folder and your 4 Oct zip: the only difference is the `detector_f_source` key gone from measured
profiles).
NOT `36397676`, which is the batch build and stays as it is, and **NEVER `17ebf114` or `b0258a7b` for the follow-up**: both carry the write bug — their re-derive dropped every record's sweep-time writes, so a tone check wrote Zip's
ratio at 1:1 and the VBC profiles said mix 0 % — and the collapse bug, where the landing read wrote a picked candidate's view
over its record; `804a0a56` and `9d3a1972` repair both from the records' own traces). It carries everything 17ebf114 did: v1.8–v2.1, the raw hold test, and the 4 Oct rules from your run (the measured pair rules, the licence row, the
out-of-scope states, the review pick, the range re-sample, the repeat repair; round 2: input-drive and one-knob amount controls, stepped by evidence; round 3: multiband names, picks on stereo units, the re-sweeps it decides itself; the 4/5 Oct night: the sidechain left unconnected with the evidence-based re-sweep, `inert`, tuner plan v2 with the v0.1 measurements, Sean's stepped rule):
targets 1..12 with the clamp to 30 and the saturation note, the reverse read, the depth-aware clamp, the vocal-anchored tone level, every deep null accounted for in `notes` (a list),
out-of-order deep points nulled before export, the shallow-break refusal, the known-licence skip and the §11 guard.
Rehearsed here as a packaged app (3 Oct evening; 4 Oct evening; again 5 Oct early on `17ebf114`, a NEW probe - the sidechain
change is in it): pre-flight finds the probe beside the executable; the four rehearsal units re-check 40 of 40 levels PASS with no
`--probe`, their landing reads and sidechain readings running live inside the same command; the projection over your zip from the
packaged app matches the build tree (20 re-sweeps, same list); C1 comp (s) swept under the OLD app was read, re-swept and exported
by the new one; NEOLD V76U73 was filed `inert` from its existing record in 3 s.

## What this follow-up does to your 3 Oct run (projected from your zip, 4 Oct)

Re-derived from your traces: your 43 exports get the deep points and the new tone checks; 15 products that
were "N threshold candidates" are decided by measurement (10 linked pairs, PuigChild 670 (s) leader/follower, Ozone 12
Vintage Main over Aux, Kiive XTComp and DSM V3 master over trims — each needs one detector load, which this mode does) and
API-2500 (m)/(s) get their lost repeat back; 15 are filed `multiband: profiling not built yet`, 8 `needs_licence`
(7 Melda + Pro-C 3: silent on every candidate), 2 `surround`; 25 stay in review, 11 of them waiting for a pick in
`cert/review_picks.json` (the sheet is `cert/review_sheet.txt`). H-Comp (m)/(s) and three others were refused for a false
licence flag: run `"$BIN" --cert-sweep-all --profile --retry-refused` once after this mode to re-sweep them.

**The sidechain (4 Oct, Kathy's ruling).** The probe used to CONNECT every sidechain input and feed it silence; WaveShell
keys from a connected sidechain, which is why C1 comp, RCompressor, SSLComp, dbx-160 and VComp read 0 dB GR on your Mac
(filed `flat`). The follow-up probe leaves those inputs UNCONNECTED, as Logic does with no sidechain source. The command
does not assume who that changes: for every product swept under the old policy that declares a second input bus (about 60
of your records), it repeats ONE of the record's own position processes at one loud level under the new probe (~2–3 s
each) and compares: within 0.1 dB the record is kept (`sidechain policy: no effect`); otherwise it is re-swept (`keys from a
connected sidechain`). A crash or a window under the new probe is written on the row and the run carries on.

**`inert` (4 Oct).** A product whose output no control moves — not even Power, Makeup or Trim — is filed `sweep result inert:
processing never runs: output unchanged by every control including Power`, never `flat`. NEOLD V76U73 is one on Kathy's Mac;
the cause is not decided (a licence state is suspected), only what was measured is said.

**Re-swept by this command, decided from your records (projected 5 Oct on `17ebf114`: 20 products — the 14 below plus the six
tuners Auto-Tune Pro / Artist / EFX / EFX+ / Access and bx_crispytuner, re-measured under tuner plan v2; plus whatever the
sidechain reading of 48 records (~3 s each) and the landing reads (~2 s each) add — ≈ 1 h 30 – 1 h 45 in all on top of the
re-derives, 13 detector loads and tone checks).** A row is
re-swept only when the plan this build makes for it differs from the plan its record was swept under: 11 that the batch
build refused at plan now have an amount control (MV2 (m)/(s) High Level, Rubber Band Compressor V2 Tension, OneKnob
Pressure (m)/(s) Pressure, RVox (m)/(s) Compression, Mike-E Comp Drive, bx_opto Pedal Density, NEOLD V76U73 Gain, Mixland
Vac Attack L/R Reduction); MaxxVolume (m)/(s) lose two switch candidates; UnFairchild's measured pick reaches 1 dB at only
5 positions and gets its refinement round. The command prints the list and each row's reason first (`RE-SWEEP: N
product(s)`), re-derives and tone-checks everything else, then sweeps those through the batch's own path and finishes
them (detector, export, tone check). OneKnob Pumper (m)/(s) and the five licence-flag refusals are not on it: the
`--retry-refused` line above covers those.

## History — Step 1 as written for the 6 Oct build 0d2ccd5d (do not follow: see "Running the 7 Oct build" at the top)

**Step 2 below lists tonight's three commands in order, and its command 1 IS this step.** Run step 2 top to bottom and nothing
twice: this section explains what command 1 does and what to expect from it.

**6 Oct: run it again over your EXISTING cert folder — no fresh start, nothing deleted.** The 5 Oct run's records are
repaired in place from their own traces (the sweep-time writes that the re-derive had dropped; the sixteen pair/pick records
that had collapsed to one sweep). The same command as before:

```
BIN=/Applications/ejmap.app/Contents/MacOS/ejmap
"$BIN" --cert-preflight
caffeinate -i "$BIN" --cert-tonecheck-all 2>&1 | tee -a ~/Library/ejmap/cert/tonecheck.log
```

It will re-run 47 tone checks and 2 re-sweeps and skip the rest (about 35–40 minutes): the 11 checks that failed on 5 Oct
(C1 comp-gate/-sc m/s, Zip, VBC FG-Red / Grey / MU / Rack, AMEK, MAGNUM-K — the first seven failed on the dropped writes);
31 passed checks whose unit declares a sidechain input, each with one extra reading under the new sidechain policy
("self-keyed, as EchoJay 04e") against its own policy — a difference over 0.1 dB re-sweeps the record; 5 that become
exportable (Auto-Tune Vocal Compressor, Shadow Hills Class A, UnFairchild, dbx-160 (s), Mixland Vac Attack); and the
re-sweeps of Mike-E (PACE: it runs with the iLok in) and MAGNUM-K (both thresholds written together — Kathy's new pick).
Put the 6 Oct `review_picks.json` into `~/Library/ejmap/cert/` first (it changes MAGNUM-K's entry).

**Stop:** Ctrl-C in that window, any time — a product mid-check leaves no result and is re-run from its start. **Resume:**
the same command. **Check:** nothing to type; the log's last `=== tone checks:` line is where it is.

**The morning zip** (the same as before; stop the run first):
```
cd ~/Library/ejmap && zip -rq ~/Desktop/ejmap-tonecheck-$(hostname -s)-$(date +%Y%m%d).zip cert
unzip -l ~/Desktop/ejmap-tonecheck-$(hostname -s)-$(date +%Y%m%d).zip | grep -c config.json      # must print 0
```

What that one command does, per exported product: re-derives the record from `cert/<identity>.sweep.processes.json` + `cert/raw/`
(keeping the detector, Rule 1's decision, the map state); re-exports `cert/profiles/<Product>.json`
with `in_at_gr_dbfs` 1–6 and `quality.deep_point_error_db`; runs the tone check at g = 2 with the v1.7
pick, then one check per deep level the profile carries (4, 5, 6), each to 0.5 dB; a failing deep level
is nulled across all positions (the profile stands); the result is embedded as `tone_check` with
`deep_levels[]` and `deep_levels_nulled[]`, and the row in `cert/outcomes.json` is rewritten.

**The test level per level (3 Oct).** Each level g (2, 4, 5, 6) is checked at the level the server would
ask at for the spec's example track, through this unit's detector: L_ref = −18.4 + f × (−6.2 + 18.4 −
3.01) with f = `detector_f` (an RMS unit −18.4, a peak unit −9.21, CL 1B −14.45); if the §6.4 pick at
L_ref fails its clamp (12 dB up to 3 dB, then 12 + 2 × (g − 3)), the nearest valid level among the
positions' `in_at_gr[g]` values is used instead. Every level records `L_ref_dbfs`, the L it was tested
at (`L_rms_dbfs`), the gap and the rule (`L_rule`). A level is
nulled across all positions only for one of two named reasons (`null_reason`): `failed_check_at_L` (the
GR missed by more than 0.5 dB at the recorded L) or `no_valid_L_clamp_geometry` (the unit's own 1→g
spacing is at least 12 dB at every position — `spacing_1_to_g_min_db` says how much — so no L inside
the clamp exists; the unit's property, not a failed check). On the four rehearsal units every level tests at
L_ref itself and passes (Lindell SBC's 6 dB included, under the v1.9 clamp).

- **Resumable:** run the same command again; a profile whose tone check already carries `spec: v1.7`,
  its deep levels and `L_ref_dbfs` is skipped; `--retry-licence` reaches only the `needs_licence` set (they
  have no result file).
- **Licence — nothing known to need one is loaded (3 Oct).** A product the scan stopped, or one with a
  `needs_licence` row in this folder, is not loaded at all (the row says *known from the scan* / *known
  from this folder's outcomes*); a window during a check makes the row `needs_licence` (the export
  stands). `--retry-licence` is the only way past, when the licence is back.
- **Section 11:** the installed version must be the record's; a record from another version (or an
  unknown version) is `needs_review` with the §11 reason and is never loaded.
- **No sweeps, nothing sent.** A record without traces (none expected from the batch) is tone-checked on
  its existing points and says so.

## History — Step 2 as written for the 6 Oct build 0d2ccd5d (do not follow)

Three commands, in this order, each in its own go; Ctrl-C any time, and each resumes from where it stopped when run again.
**Command 1 is step 1 (the follow-up) — not a second run of it.** Running step 2 top to bottom does everything once.

```
BIN=/Applications/ejmap.app/Contents/MacOS/ejmap
"$BIN" --cert-preflight
# 1. the compressor follow-up (step 1 above) - about 35-40 minutes
caffeinate -i "$BIN" --cert-tonecheck-all 2>&1 | tee -a ~/Library/ejmap/cert/tonecheck.log
# 2. the gain and timing data for every certified compressor, again, under Kathy's two specs - about 1 hour
caffeinate -i "$BIN" --phaseb-all --redo gain-cal,timing 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log
# 3. if there is time: the products the lexicon could not name, measured again with measurement nominating - several hours, resumable
caffeinate -i "$BIN" --phaseb-all --redo nothing_nominated 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log
```

What 2 does: re-runs exactly the gain-cal and timing rows (69 each; every other Phase B row untouched) — gain controls judged
as the gain spec says, each writable one re-measured at +3 and −3 dB (pass within 0.2 dB), an `ej_gain_profile/1`-shaped draft
per product in `cert/phaseb/gaincal/gain-cal/`; timing with the 4 kHz second pass for fast attacks, the hold scaled to slow
ones, the steady-GR shift at every position, the picked candidate as the amount on the 17 that were skipped, and a `time`
draft in `cert/phaseb/timing/timing/`. Data only: no profile is exported or changed by it. Measured here: ~50 s a product for
gain-cal, ~15 s for timing — on your Mac (2–3× faster on 5 Oct) about 50 minutes for all 69; here it would be 1.5 h.

What 3 does: the 205 rows that finished with no record on 5 Oct (91 EQs, 114 saturators / amp sims — the names did not match)
are run again; the mode probes every control at its ends and nominates the ones that move the measure. Measured here: 30–340 s a
product (an amp sim with five tone controls is the slow end) — all 205 is a night of its own (5–12 h here, less on yours); stop
it in the morning, it resumes the next night. Hang guards as before (EQ 30 min, saturation 15 min a product).

`"$BIN" --phaseb-status` says where 2 or 3 is at any time. The morning zip is as in step 1 (stop the run first).

## History — Step 2 as written for the 5 Oct build b0258a7b (do not follow)

Step 1 (the follow-up above) is unchanged and runs first, once. Step 2 runs after it, over several nights: it measures
every installed EQ, limiter, de-esser, saturator and amp sim, reverb, delay, transient shaper, gate and multiband, plus a
gain calibration and a timing read of every certified compressor, in that order — one product at a time, in its own
process, into `~/Library/ejmap/cert/phaseb/`. Nothing is exported or sent; the folder goes back in the morning zip.

**Start (and resume — the same command every night):**

```
BIN=/Applications/ejmap.app/Contents/MacOS/ejmap
caffeinate -i "$BIN" --phaseb-all 2>&1 | tee -a ~/Library/ejmap/cert/phaseb.log
```

It prints what it found per category and a line per product as each finishes:
`[eq 12/175 | all 120/703] bx_digital V3: ok 93 s | elapsed 3:10:00 | ETA 9:40:00`. A product already done is never run
again, so the same command picks up where it stopped. `caffeinate -i` keeps the Mac awake with the lid open; a closed lid
still sleeps it (a product measured across a sleep is marked `slept` and not trusted).

**Stop:** press Ctrl-C in that window. Any time is fine — a product that was mid-measurement leaves nothing behind and is
measured again from its start on the next run.

**Check (from any window, without loading anything):**

```
"$BIN" --phaseb-status
```

prints done / total per category, time spent, the ETA, and what is next.

**Each morning, the zip** (stop the batch first, Ctrl-C) — whole `cert/` the first morning, then only what is new (so the zips stay small):

```
cd ~/Library/ejmap
# first morning:
zip -rq ~/Desktop/ejmap-phaseb-$(hostname -s)-$(date +%Y%m%d).zip cert && touch cert/phaseb/.zipped
# every later morning:
find cert -type f -newer cert/phaseb/.zipped | zip -q ~/Desktop/ejmap-phaseb-$(hostname -s)-$(date +%Y%m%d).zip -@ && touch cert/phaseb/.zipped
# the check, every time (must print 0 - config.json is never in the zip):
unzip -l ~/Desktop/ejmap-phaseb-$(hostname -s)-$(date +%Y%m%d).zip | grep -c config.json
```

**How long, how big (measured here on 5 Oct, two products per category, projected over your census).** Per product:
gain-cal ~35 s, timing ~10 s, limiter ~45 s, EQ ~75 s, de-esser ~40 s, saturation ~70 s (6 s to 135 s), reverb ~100 s (12 s
to 200 s), delay ~110 s, transient shaper ~75 s, gate ~65 s, multiband ~190 s. Over your counts (43 certified compressors,
46 limiters, 175 EQs, 22 de-essers, 193 saturators + amp sims, 100 reverbs, 38 delays, 18 transient shapers, 8 gates, ~17
multibands): about **14 hours in all** — roughly two nights; the spread is wide (5 to 19 hours) because a product's time is
its control count. A product that hangs is cut off at its category's limit (10 to 30 minutes) and recorded `timed out` with
what it had; the batch moves on. Licence-bound products are skipped without a load. Size: the traces are gzipped; about
**125 MB after night 1** (through the saturators) and **about 550 MB at the end** — most of it reverbs and delays (3 MB a
product). The later-morning zip holds only that night's share.

## Tube-Tech CL 1B

**If tonight's batch certifies CL 1B on Sean's Mac, his fresh record is the one that is used** — the
follow-up re-derives and tone-checks it like every other exported product, and nothing from this repo
is involved.

If it does not (the iLok was away at his scan, so CL 1B is `needs_licence`; or the sweep refused), the
committed traces from here can be brought into his tone-check folder — the record, its process list and
the 62 raw captures, 650 KB — and the follow-up then re-derives and tone-checks CL 1B on his Mac from
them, with the iLok present:

```
# on the building Mac, from the repo:
tools/ejmap/packaging/export_traces.sh AudioUnit_517f614e_2.5.62     tools/ejmap/cert-traces/2026-10-02-batch10-profile/refined ~/Desktop/cl1b-traces.zip
# on Sean's Mac, before the follow-up command (-n: never overwrites his own files):
cd ~/Library/ejmap && unzip -n ~/Desktop/cl1b-traces.zip
"$BIN" --cert-tonecheck-all --retry-licence 2>&1 | tee -a ~/Library/ejmap/cert/tonecheck.log
```

`--retry-licence` is needed only because his scan stopped CL 1B (a known licence stop is otherwise not
loaded); with the iLok in, the check runs. **The §11 guard is in the binary:** the traces are of CL 1B
**2.5.62**; if his installed CL 1B is any other version the row is `needs_review` with *installed
version X differs from the record's 2.5.62 (section 11)* and nothing is loaded — rehearsed here on an
edited record. If CL 1B is not installed at all, the log says it resolves to no component and the row is `needs_review`. The imported
record is keyed by its identity, so his own CL 1B record (if any) is never overwritten (`unzip -n`).

## About how long

Measured here on 2 Oct on four exported products (traces from the rehearsal): **17 s in all** —
re-derivation, export and four probe processes each (g = 2 plus 4/5/6; the probe renders offline, so a
nine-hold tone process is about a second). Call it **10–15 s per exported product** (ten tone processes each since v1.10): a run that
exported 60 products is about fifteen minutes. It can be stopped and resumed.

## What he zips back

Exactly as before: `cd ~/Library/ejmap && zip -rq ~/Desktop/ejmap-tonecheck-$(hostname -s)-$(date +%Y%m%d).zip cert`
— `cert/` only, never `config.json`. The profiles, their `.tonecheck.json`, the re-derived records and
`outcomes.json` are all inside.

**Review picks (4 Oct).** Put Kathy's `review_picks.json` into `~/Library/ejmap/cert/` before running. A picked candidate
on a stereo unit (VBC Rack's MU Threshold Left, MAGNUM-K's Threshold 1) gets the same tone check as a measured pair: the
server's write order (engage, neutral including the other candidates, ratio, amount last) and both output channels within
0.5 dB of g.

**Corrected control data (4 Oct).** For every loaded product whose sampled range left a gap (an end that prints a word,
a missing end sample, or an instantiate value outside the sampled ends), the follow-up reads the control at 21 norms and
writes `cert/controls/<identity>.controls.json` — the full taper and the corrected range. It is in the zip you send back;
nothing is published from EJ Map. It adds roughly 15 s per such control.

