# Re-check of the held compressors - Sean's Mac, tonight (16 Oct 2026)

**What this does:** it re-checks only the 46 units in `held_units.txt`, using your existing certification folder (`~/Library/ejmap/cert`). Each unit's profile is rebuilt from your traces by the current build, then tone-checked again (2 / 4 / 5 / 6 dB). Any accuracy pass a unit is missing then runs. Nothing else in your folder is re-run.

Nothing is sent anywhere. **Never press Send** in EJ Map.

**The units:**
- the 38 checked on 5 Oct;
- AMEK Mastering Compressor, SPL IRON and Maag MAGNUM-K;
- VBC FG-Grey, VBC FG-MU and VBC Rack;
- Auto-Tune Vocal Compressor;
- Pro Audio DSP DSM V3 (it changed in the last build).

**The build:** `ejmap.app` in this folder, from `feat/ejmap-cert` at **26b3cbaf**. It is signed by Developer ID 8BT5F9B887 (not notarised). It replaces b7aa304c. Its results match (checked on Kathy's Mac against your cert 5 folder: 0 unexplained differences, all 1307 drafts identical), and a bridged plugin is loaded once per pass instead of once per position.

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
- **The steps:** preflight, followup, accuracy_gaps_drafts, accuracy_gaps, drafts.
- **Time:** the plan prints about 2 h 10 min at 2 jobs (dry-run on a copy of your cert 5 folder). Allow 3 h. Auto-Tune Vocal Compressor's gain and timing are ~1 h of that.
- **Logs:** in `~/Library/ejmap/cert/run_all/` and `~/Library/ejmap/cert/recheck_16oct.log`.

## 3. Leave it
- It keeps the Mac awake and **stops by itself at 07:00**.
- If it stops for any reason (07:00, a power cut, Ctrl-C), run the same command again. It carries on where it stopped, and nothing is measured twice.

## 4. In the morning: zip what is new and send it to Kathy

```
~/Downloads/SEAN_RECHECK_PACK_16oct/ejmap.app/Contents/MacOS/ejmap --zip ~/Library/ejmap/cert --out ~/Desktop/ejmap-cert-recheck-$(date +%Y%m%d).zip --since marker
unzip -l ~/Desktop/ejmap-cert-recheck-*.zip | grep -c config.json      # must print 0
```

Email that zip to Kathy. It holds results only: no plugin files, no audio, no sign-in token.

## If something looks wrong
Send Kathy the last 40 lines of `~/Library/ejmap/cert/recheck_16oct.log`. Do not delete or move anything in `~/Library/ejmap`.
