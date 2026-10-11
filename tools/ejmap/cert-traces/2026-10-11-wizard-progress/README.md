# Wizard progress bar: rehearsal, 11 Oct (feat/ejmap-wizard)

The rehearsal used the same 8 plugins as the 10 Oct empty-store run: Lindell 254E / 6X-500 / SBC, Maag EQ2 / EQ4, bx_opto, elysia mpressor and bx_townhouse. Conditions:
- an EMPTY map store (`--ejmap-ledger`) and the real home folder;
- `--jobs 2 --measure --bundle`, run inside a pseudo-terminal (`script`) so that the live line is drawn as a user would see it;
- the real `~/Library/ejmap`: 1123 maps before and after, and 0 files changed under it.

**Run 1** stopped with Ctrl-C (SIGINT to the run's process group) 20 s after the first plugin landed. **Run 2** was the same command, which resumed. **Run 3** was the same command once more, with everything already landed.

## What the terminal shows (selected frames, `run1_frames.txt` / `run2_resume_frames.txt` have every distinct frame)

```
EJ Map wizard - every detail goes to <scratch>/wiz7/work/wizard.log
[--------------------]   0%  Scanning 1 of 8 - Lindell 254E (2 at a time) - working out the time
[#-------------------]   6%  Scanning 6 of 8 - bx_opto (2 at a time) - working out the time
[###-----------------]  15%  Measuring 1 of 4 - about 9 min left
[#####---------------]  22%  Measuring 1 of 4 - bx_opto (2 at a time) - about 8 min left
[#######-------------]  35%  Measuring 1 of 4 - bx_opto (2 at a time) - about 7 min left
[##########----------]  47%  Measuring 1 of 4 - bx_opto (2 at a time) - about 6 min left
[############--------]  60%  Measuring 1 of 4 - bx_opto (2 at a time) - about 4 min left
[#############-------]  66%  Measuring 2 of 4 - bx_opto (2 at a time) - about 4 min left
^C
[########------------]  41%  Stopped
Stopped at 41%: 1 of 4 measured, 3 left (the 2 being measured start again). Nothing was packed or sent. Run the same command to carry on - it starts where it stopped.

$ (the same command)
EJ Map wizard - every detail goes to <scratch>/wiz7/work/wizard.log
[##------------------]  10%  Scanning 8 of 8 - working out the time
[########------------]  41%  Measuring 2 of 4 - bx_opto (2 at a time) - about 6 min left
[###########---------]  53%  Measuring 2 of 4 - bx_opto (2 at a time) - about 5 min left
[################----]  77%  Measuring 2 of 4 - bx_opto (2 at a time) - about 3 min left
[###################-]  94%  Measuring 4 of 4 - bx_opto (2 at a time) - under a minute left
[###################-]  97%  Packing 1 of 1 - the contribution bundle - under a minute left
[####################] 100%  Done
Done in 6 min 47 s: 8 plugin(s) - 4 plugins ready, 3 newly measured, ready to send, 1 could not be measured. Ready to send: contribution.zip (3 new, 2 confirmation(s), 21.5 KB) - nothing was sent. Details: wizard.log
```

Notes on the run:
- **The stop.** The bar drops from 66% to 41% when stopped. This is by design: the two plugins in flight are discarded (Phase B re-runs them), so the stop reports what was kept. It is exactly where the resume opens, and the line says why.
- **The resume** opens Scanning at 10%, because all 8 plugins were already mapped, and Measuring at 41%, because 1 of the 4 had landed.
- **The ETA:** the first measuring frame said "about 9 min". From there, run 1 + run 2 took about 13 min of measuring, including the work lost at the stop. Run 2 alone said "about 6 min" and took 6 min 47 s in total.
- **Run 3** (`run3_all_landed_frames.txt`) jumps 10% → 97% → 100% in 20 s.

## progress.json (what a window app reads)

- After the stop (`progress_after_stop.json`): `percent 41.4, stage "Stopped", stopped true, eta_words "stopped"`.
- At the end (`progress_final.json`): `percent 100, stage "Done", problems 1, eta_words "finished"`, plus the summary.
- `seconds_per_plugin`: compressor 349.3 s measured, EQ 35.6 s measured, check 1.5 s measured. Map shows 4.0 s "starting figure", because no plugin was mapped in run 2.

## Defects the rehearsals found (all fixed, pinned and mutant-tested)

| run | what was wrong | pin |
|---|---|---|
| wiz3 (`defect_wiz3.txt`) | After Ctrl-C mid-measure, it packed a bundle with "3 measuring" and said "Done" | WZ-PROG-5 |
| wiz5 | After the stop, progress.json said 100% / Done (only the summary said Stopped) | WZ-PROG-5b |
| wiz5 | The bar sat still for 6 minutes while two compressors ran: only landed plugins counted. A running plugin now earns its time, up to 90% | WZ-PROG-1 |
| wiz6 (`defect_wiz6.txt`) | "Stopped at 66%", then the resume opened at 41% | WZ-PROG-5c |
| wiz7 | progress.json `problems 0` while the summary said "1 could not be measured" (an EQ that landed ok and then failed its acceptance) | WZ-PROG-6 |

Pins WZ-PROG-1..6 are in RoundTripTest.cpp; the suite is at 4077 checks, 0 failures. Every mutant went red:
- the backwards guard removed;
- overlapping bands;
- the ETA ignoring measured times;
- the ETA ignoring --jobs;
- a running plugin counted as half (the old rule);
- resume not marking landed rows done;
- stop as any non-zero exit;
- a stopped run written as Done;
- a stop keeping the running credit;
- the tracker keeping the held bar on a stop (it survived at first; the pin was strengthened with `ranFor`);
- not_measurable not counted as a problem.

## Not pinned (exercised by this rehearsal only)

- The run code's wiring into Phase B: the row hooks, `resumeItems` on the landed rows, and the stop path's `return 130`.
- The cout redirect to wizard.log.
