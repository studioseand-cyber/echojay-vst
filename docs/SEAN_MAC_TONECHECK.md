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

## Next build — NOT YET: run only when Kathy says the next build is yours (bcc4eea9; the build above, dc77d0a5, is the fallback)

Built from commit **bcc4eea9** (packaged and signed at ~/Desktop/ejmap-dist-8oct-c; derive-only EQUAL to dc77d0a5 on your current folder, your 4 Oct
zip, cert_sc and cert_tc35 - 0 differences in rows, records, profiles and controls, from the build tree and again from the packaged
binary, so the compressor path above is untouched). It supersedes 99f80d36, d0587ff4 and ac9e3bfb. It adds the six new specs (limiters, EQ,
de-essers, saturation + amp sims, reverb / delay, transient shapers / gates), the channel strips' and multiband drafts, a DRAFT
profile for every Phase B category in `cert/phaseb/<category>/drafts/` (never `cert/profiles`), the one-command run and the review's
Phase B sections. Install it the same way as the build above AFTER NIGHT 1 above (Nights 2 and 3 are not run on dc77d0a5: their
strips and gain-all are in this line, and their nothing_nominated rows are all re-run by this line's category redos), then ONE
command, every night:

```
caffeinate -i "$BIN" --run-all --steps preflight,multiband,limiter,deesser,eq,saturation,reverb_delay,transient_gate,gain_all,strips,drafts --until 07:00
```

It runs those steps in that order, each step's output in `cert/run_all/<step>.log`, one progress line a minute (done / total, elapsed,
ETA, the hour it stops). At 07:00 it stops the step it is in (that step's finished products are kept) and starts nothing new; the SAME
line the next night carries on from there (a step it stopped resumes without re-deleting what it already redid). Ctrl-C works the
same way. `--dry-run` on the end prints what it would run and changes nothing. About 30 hours in all (the dry-run's ETA 30:08):
multiband ~20 min, limiters ~25 min, de-essers ~10 min, EQ ~2.5 h, saturation ~2 h, reverb / delay ~3.2 h, transients / gates ~32 min,
gain-all ~7 h, strips ~14 h, drafts ~1 s. With 21:00-07:00 nights: night A runs everything up to and including transients / gates
(~9.1 h) and starts gain-all; night B finishes gain-all (~6.1 h) and starts the strips; night C runs the strips; night D the last few
minutes of the strips and the drafts. The steps always run in that order, whatever order `--steps` lists them in.

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
`--since marker`. Saturation runs BEFORE gain-all in this line, so it chooses its output control without the gain-all drafts (from the
controls named output / volume / level / trim / makeup and the unnamed pool, each checked to be level-only after the drive).

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

