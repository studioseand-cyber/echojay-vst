# Re-check of the held compressors, then multiband v1 and the strips - Sean's Mac, from tonight (16 Oct 2026)

**What this does:** it re-checks only the 46 units in `held_units.txt`, using your existing certification folder (`~/Library/ejmap/cert`). Each unit's profile is rebuilt from your traces by the current build, then tone-checked again (2 / 4 / 5 / 6 dB). Any accuracy pass a unit is missing then runs. Nothing else in your folder is re-run.

**Then, in the same command (part 2):**
- **Multiband v1:** every multiband in your folder. Per band: a tone centred in the band checked at 2 / 3 / 4 dB, the threshold tracked across levels, the make-up control. Per crossover: the frequency where the two neighbouring bands' responses are equal (each band soloed where the plugin has solo buttons, otherwise isolated), checked against the display within 1/6 octave. About 3-5 min a unit.
- **The channel strips:** your folder already has them, measured on 13 Oct, so this step is skipped. It runs only on a folder without them. The drafts step at the end redraws every strip (and everything else) from your records with this build.
- **Time for part 2:** about 1 h (dry run on a copy of your cert 5 folder).

Nothing is sent anywhere. **Never press Send** in EJ Map.

**The units:**
- the 38 checked on 5 Oct;
- AMEK Mastering Compressor, SPL IRON and Maag MAGNUM-K;
- VBC FG-Grey, VBC FG-MU and VBC Rack;
- Auto-Tune Vocal Compressor;
- Pro Audio DSP DSM V3 (it changed in the last build).

**The build:** `ejmap.app` in this folder, from `feat/ejmap-cert` at **ad50ddbb**. It is signed by Developer ID 8BT5F9B887 (not notarised). It replaces b7aa304c and 26b3cbaf. Its results match yours (checked on Kathy's Mac against your cert 5 folder: 0 unexplained differences, all 1307 drafts identical), and a bridged plugin is loaded once per pass instead of once per position.

## Before you start
- Mains power, lid open.
- One Waves version installed, as for the certification.
- If a licence or activation window appears and stays, do not click it. Quit the app (Cmd-Q) and run the same command again.

## 1. Unzip and clear the download flag (once, 10 seconds)
Unzip `SEAN_RECHECK_PACK_16oct.zip` into Downloads. Then in Terminal:

```
xattr -dr com.apple.quarantine ~/Downloads/SEAN_RECHECK_PACK_16oct
xattr -lr ~/Downloads/SEAN_RECHECK_PACK_16oct | grep -c quarantine      # must print 0
```

## 2. Run it (one command)

```
bash ~/Downloads/SEAN_RECHECK_PACK_16oct/recheck.sh
```

It runs the preflight first:
- The preflight checks mains power, the probe beside the app and its signature, and the download flag.
- **If it refuses, nothing runs.** It says why. Fix that and run the same command again.

It then starts the re-check and prints the plan:
- **The steps (part 1):** preflight, followup, drafts, then the two accuracy-gap steps through `recheck_gaps_18oct.sh`.
  - **Correction, 18 Oct:** the original part 1 named the run-all steps accuracy_gaps_drafts and accuracy_gaps.
  - Both pass `--redo accuracy_gaps`, which this build (ad50ddbb) refuses with "not a Phase B category" (exit 2). On Sean's cert 6 both failed and the rest ran.
  - The dry runs below printed the plan but never parsed it, so they could not catch this.
  - `recheck_gaps_18oct.sh` runs the same work in values ad50ddbb accepts (`--redo unfinished` for gain-cal + timing, `--redo nothing_nominated` for combined + material + frequency).
  - Later builds take `accuracy_gaps`: one list, and every run-all `--redo` value is pinned by WZ-REDO-1.
- **The steps (part 2):** multiband_v1, strips, drafts. Part 2 starts by itself when part 1 ends.
- **Time:** the plan prints about 2 h 10 min at 2 jobs (dry-run on a copy of your cert 5 folder). Allow 3 h. Auto-Tune Vocal Compressor's gain and timing are ~1 h of that. Part 2 then takes about 1 h. In all, about 4 h.
- **Logs:** in `~/Library/ejmap/cert/run_all/` and `~/Library/ejmap/cert/recheck_16oct.log`.

## 3. Leave it
- It keeps the Mac awake and **stops by itself at 07:00**.
- If it stops for any reason (07:00, a power cut, Ctrl-C), run the same command again. It carries on where it stopped, and nothing is measured twice.
- If 07:00 comes before part 2 ends, run the same command the next evening; anything finished is not measured again.
- Each morning, do step 4 (it sends only what is new since the last zip).

## 4. In the morning: zip what is new and send it to Kathy

```
~/Downloads/SEAN_RECHECK_PACK_16oct/ejmap.app/Contents/MacOS/ejmap --zip ~/Library/ejmap/cert --out ~/Desktop/ejmap-cert-recheck-$(date +%Y%m%d).zip --since marker
unzip -l ~/Desktop/ejmap-cert-recheck-*.zip | grep -c config.json      # must print 0
```

Email that zip to Kathy. It holds results only: no plugin files, no audio, no sign-in token.

## If something looks wrong
Send Kathy the last 40 lines of `~/Library/ejmap/cert/recheck_16oct.log`. Do not delete or move anything in `~/Library/ejmap`.
