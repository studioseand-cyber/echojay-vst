# Parallel probes on one Mac (10 Oct, this Mac: 10 cores, 16 GB, background load average ~4.3)

Harness: `pass.sh <pass> <N>` runs the 8 units in `units.txt` (one ejmap process each, its own scratch cert folder, the signed
probe) through an N-slot pool; `compare.py A B` compares every JSON figure (records, profiles, drafts, rows) and every trace number,
timing fields excluded (*_ms, seconds, stamps, landing slices / confirm). Passes: serialA, serialB (the run-twice control), par2, par4.
Build tree after 7a68f1a7. Units: bx_opto (the full compressor path: sweep, detector, tone check, export), Maag EQ4, CamelCrusher,
bx_delay2500, MReverb, Unfiltered Audio G8, Transient Master, SPL De-Esser.

## Readings
| unit (mode) | serialA vs serialB | serialA vs par2 | serialA vs par4 |
|---|---|---|---|
| all 8: records / profiles / drafts / rows (JSON figures) | 0 differences | 0 | 0 |
| traces, 7 of 8 units | 0 | 0 | 0 |
| SPL De-Esser trace | 1 (`pos slices`: a landing count, timing) | 1 (same field) | 1 (same field) |
| MReverb trace | 0 | 1 trace of 201: from t = 2005.5 ms of `time0.833`'s tail (input silent) the output jumps from -189 dB to a flat -45 dB for ~3 s | 0 |
| G8 trace | 0 | 0 | 1 trace: `q0.n0.b` two windows longer (341 vs 339 lines) |

- **MReverb**: an output appearing out of silence at a flat level, in one run only, is the plugin's own injection - consistent with
  Melda's unlicensed/demo behaviour on this Mac (MLimiterX here: "intermittent dropouts"). INFERRED, not confirmed: the licence state
  was not read. It is wall-clock driven, so any schedule change moves it (serial included); the record's figures did not change.
- **G8**: the `q0.n0` ramp ends at ~1.4 s with no `stage done` in EVERY pass (serial too) - the probe process ends there (G8's last
  crash reports on this Mac, 5 Oct, are SIGABRT inside com.UnfilteredAudio.G8; none written today). Under 4 jobs two more buffered
  windows were flushed before the end. Not a parallel effect; a separate finding (a silent truncated trace).

**Verdict: no mode showed a reading change beyond the run-twice noise.** Coverage: these 8 modes, one run each at N = 2 and 4;
NOT tested: gain-all, timing, limiter, multiband, tuners, strips, combined / material / frequency / samplerate, CPU-heavy units.

## Speed and load
| pass | wall | sum of unit times | peak RSS (ejmap + probes) | peak CPU | timeouts / dropouts |
|---|---|---|---|---|---|
| serialA | 719 s | 718 s | 0.12 GB | 76 % (of 1000) | none |
| serialB | 705 s | 705 s | 0.12 GB | 71 % | none |
| par2 | 384 s | 713 s | 0.13 GB | 68 % | none |
| par4 | 383 s | 717 s | 0.33 GB | 133 % | none |

Each unit's own time is unchanged in parallel (x0.95 .. x1.04): no contention at these loads. par2 = par4 because bx_opto alone
takes 383-387 s (the critical path); on a full category (tens of rows) the speed-up approaches N for the parallel lane. (CPU / RSS
sampled every 2 s, so short probe processes may be under-counted.)

## --jobs N (design, not built)
- **Where**: inside a Phase B step (rows in parallel), never across run-all steps - the step order (priority), run_all.json and
  the night lines stay as they are. `--run-all --jobs N` passes `--jobs N` to every --phaseb-all step child; the follow-up
  (--cert-tonecheck-all), categorise, preflight and drafts stay serial.
- **Rows**: an N-slot pool over the category's work list. Each product already has its own `.tmp-<stem>` folder and child; the
  PARENT stays the only writer: it moves a finished child's files into place, writes the row (atomic) and progress/summary one at a
  time from a completion queue. Completion order varies; rows are keyed by product, so nothing depends on it.
- **The serial lane** (run alone, the pool drained first): UAD (shared DSP + the Satellite), every product the licence file
  governs or the gate marks demo, PACE / iLok vendors (SSL Native, Soundtoys, kHs, Gold Clip, APB, ...), and any product whose
  last row was window / needs_licence / timed_out / unhostable / silent_output. Why: the window watch counts windows in the
  CHILD'S process tree, but a PACE / iLok window belongs to a helper outside it - with other jobs running it could not be pinned
  to a product. Measured share of Sean's work in that lane: 26 % (6.2 of 23.5 h; delays 88 %: Soundtoys / UAD).
- **Hang guard**: per child, as now (runChild's timeout + the declared GUARD). Mac sleep: each child's own slept_ms, as now.
- **Resume**: unchanged - a row exists only when whole; on restart every .tmp-* is deleted and its child killed by folder path.
  `--until` / Ctrl-C: no new child starts; running ones are stopped with the step's process group; their (up to N) rows re-run.
- **Memory**: N capped by `--jobs` (default 1); refuse to start a job while free + inactive memory is under ~1.5 GB.
- **Proof before trusting it on Sean's Mac**: one night at N = 2 with a known control set (these 8 or rows already measured)
  re-run serially and compared with compare.py; N = 4 only after that is clean.

## Nights left (estimate)
Remaining after tonight's serial night 1: ~30.6 h of measured-rate work (nights 2-6 of the runbook), of which ~2.1 h stays serial
by design (the new build's one follow-up, categorise) and ~28.5 h is Phase B rows, ~30 % of them in the serial lane.
| N | after night 1 | nights at ~5 h (Kathy's 7-night basis) | incl. tonight |
|---|---|---|---|
| 1 | 30.6 h | ~6 | ~7 |
| 2 | 2.1 + 28.5 x (0.3 + 0.7/2) = ~20.6 h | ~4 | ~5 |
| 4 | 2.1 + 28.5 x (0.3 + 0.7/4) = ~15.6 h | ~3 | ~4 |
Assumes per-row times hold at N (measured here only at light loads) and Sean's Mac has the same headroom.

## The untested modes (10 Oct evening, Kathy's BUILD --jobs item 2) - modes2/
Same harness, serial twice then 2 and 4 at once. In set a, multiband / tuners / timing were mis-chosen (0 rows: those categories'
work lists need an outcomes row / the ledger category here) and re-run in set b with C6 (s) under a seeded `multiband` outcome,
bx_crispytuner through --cert-tuner, and timing on a Lindell SBC certified first (seed: --cert-limiter-comp, tone check PASS).

| mode (unit) | serial vs serial | serial vs 2 | serial vs 4 | verdict |
|---|---|---|---|---|
| strips (bx_console N, 9.4 min) | 440 record figures, 44,868 trace numbers (EQ centre up to 2.2 kHz) | 471 / 44,891 | 450 / 44,887 | NOT REPEATABLE EVEN SERIALLY: parallel cannot be shown within the noise -> KEPT SERIAL (kSerialModes) |
| limiter_comp (bx_limiter, 6 min) | 0 / 0 | 0 / 0 | 0 / 0 | parallel |
| gain-all (CamelCrusher) | 0 / 0 | 0 / 0 | 0 / 0 | parallel (CamelCrusher itself is serial-lane: PACE state unknown) |
| multiband (C6 (s), 4 min) | 0 records; 33 trace numbers, all landing slices | 0; 34 slices | 0; 32 slices | parallel |
| tuners (bx_crispytuner, 2 min) | 0 records; 17,073 trace numbers (per-window pitch, out_cents) | 0; 10,000 | 0; 17,083 | parallel (records identical; traces jitter as much serially) |
| timing (Lindell SBC, every Release position + the 4 kHz pass) | 0 / 0 | 0 / 0 | 0 / 0 | parallel |
| timing (bx_opto, set a: program dependence only) | 0 / 0 | 0 / 0 | 0 / 0 | parallel |

The strips' noise is the unit's own output varying run to run (inferred: bx_console's analogue noise / tolerance modelling, not
established). A strip that repeats serially is needed before strips can run in parallel. Peak RSS (ejmap + probes) <= 0.29 GB, CPU <= 119 %.
