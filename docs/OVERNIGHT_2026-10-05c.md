# Run 4 (5 Oct 2026, evening) — the Phase B batch for Sean's run: report

**17ebf114 untouched; nothing sent.** 36397676 untouched. `PACKAGING_EJMAP_APP.md` and `SEAN_MAC_TONECHECK.md` were edited
only after step 6 passed, and both still name 17ebf114 as the fallback. Seven commits on `feat/ejmap-cert`, e0f68544 →
c4eb878a, every one through the gate, pushed. Suite 3598 → 3644 checks. Live runs in scratch only; `~/Library/ejmap/cert`
does not exist here; no licence-bound product loaded; 46 GB free. **The 18:00 cut was not met: the packaged build was signed
at 18:06 and its docs landed at 18:12** — the live step-6 comparison (two full follow-up runs, 22 min) ran first, by the rules.

## The build

**b0258a7b**, packaged at `~/Desktop/ejmap-dist-phaseb/ejmap.app` (both signatures Developer ID, team 8BT5F9B887; the stamp
in the binary is `b0258a7b`, not `-dirty`; preflight from inside the bundle exit 0; one product through `--phaseb-all` from the
packaged app, `--phaseb-status` read it back, its traces open with `gunzip`).

| item | commit | what |
|---|---|---|
| ruling (c) | e0f68544 | the text pass samples only the controls a mode needs (`--text-at i,j,k`), timeout scaled to the count; Saturn 2: 58 of 951 in 15 s, 46 s in all, Band 1 Drive confirmed |
| the batch | 8161b8fe | `--phaseb-all` / `--phaseb-status`: discovery by category, priority order, hang guards per category (10–30 min, stated), atomic rows, progress line + `progress.txt`, summary.json, raw gzipped |
| kill tests | 38fddd31 | SIGINT (group), SIGKILL (parent only — the orphan is killed on resume), SIGINT mid tone check in the follow-up: no half-done record, re-measured once. **Found two things** (below) |
| ruling (d) | 65d9e1ef | the digest's appendix: unnamed controls per category, one line each; not sent |
| step 6 | b0258a7b | the evidence file: derive-only over Sean's zip (0 differences), the rehearsal set derive-only (0) and LIVE (two intended differences, one product no longer re-swept twice, the rest time stamps) |
| step 7 | c4eb878a | PACKAGING names b0258a7b (17ebf114 the fallback); SEAN_MAC_TONECHECK.md step 2: start / stop / check / resume / morning zip |

## Two things the kill tests found

1. **The sidechain re-sweep repeated — in 17ebf114.** `sidechainPolicyCheck` read the verdict cached on the record before the
   record's own policy, and the re-sweep carries the cached verdict over: a product re-swept under `unconnected` was re-swept
   on EVERY follow-up run (C1 comp (s), 318 s each; the kill test's third and fourth runs showed it). On Sean's Mac this would
   have cost ~5 min per sidechain-keyed product per follow-up run. Fixed (the record's evidence first), pinned, shown derive-
   only on the folder: fixed build 0 re-sweeps, 17ebf114's order 1. No certification result changes.
2. **The traces were not gzip files.** JUCE's compressor at `windowBits 0` writes a bare zlib stream; `gunzip` refused every
   one. Fixed (`15 + 16`), pinned on the magic bytes.

## Rehearsal (two products per category) — time per product

| category | products | seconds each | median |
|---|---|---|---|
| gain-cal | mpressor, SBC | 52, 19 | 35 |
| timing | mpressor, SBC | 11, 10 | 10 |
| limiter | bx_limiter TP, L2 | 20, 70 | 45 |
| eq | bx_digital V3, AMEK EQ 200 | 93, 57 | 75 |
| de-esser | DeEsser, Lindell 902 | 67, 16 | 41 |
| saturation | J37, BIG AL | 135, 6 | 71 |
| reverb | Abbey Road Plates, Valhalla | 196, 12 | 104 |
| delay | bx_delay2500, H-Delay | 44, 170 | 107 |
| transient | Smack Attack, Transient Master | 135, 12 | 74 |
| gate | C1 gate, G8 | 96, 33 | 65 |
| multiband | C4, LinMB | 180, 195 | 188 |

22 products, 27 min 12 s, all `ok`; 19 MB (reverb 2.6 MB a product, delay 3.5, transient 1.4, gate 0.7, eq 0.5, the rest ≤ 0.2).

## Projection for Sean's Mac (his census: eq 175, limiter 46, de-esser 22, saturation 118 + amp_sim 75, reverb 100, delay 38, transient 18, gate 8; 43 certified compressors; ~17 multibands)

| category | n | at the mean | range (min–max per product) |
|---|---|---|---|
| gain-cal | 43 | 25 min | 14–37 |
| timing | 43 | 8 min | 7–8 |
| limiter | 46 | 34 min | 15–54 |
| eq | 175 | 3.6 h | 2.8–4.5 h |
| de-esser | 22 | 15 min | 6–25 |
| saturation + amp | 193 | 3.8 h | 20 min – 7.2 h |
| reverb | 100 | 2.9 h | 20 min – 5.4 h |
| delay | 38 | 68 min | 28–108 |
| transient | 18 | 22 min | 4–41 |
| gate | 8 | 9 min | 4–13 |
| multiband | 17 | 53 min | 51–55 |
| **total** | **703** | **~14 h** | **~5–19 h** |

Two nights at ~8 h; night 1 ends inside the saturators (gain-cal → de-esser is ~5 h). Zip growth: ~125 MB after night 1,
**~550 MB at the end** (gzipped already; most of it reverbs and delays) on top of his 72 MB zip — past 500 MB, so the runbook's
later-morning zip takes only what is new (`find -newer` a marker; `--since` by marker, not a flag). The spread is wide
because a product's time is its control count (Saphira-class saturators and 900-parameter units sit at the top).

## Waiting on Kathy

- Whether the sidechain-repeat fix changes the plan for Sean's **step 1**: if his follow-up has already run on 17ebf114, every
  sidechain-keyed product that was re-swept would be re-swept again by a second 17ebf114 run; b0258a7b does not.
- The trace size: ~550 MB is the 1 ms tail windows of reverbs and delays (80–120 runs a product). Halving the window or the
  run count is a measurement change, not made tonight.
- The digest and its appendix: still not sent.

## Blocked / not done

- The Artist settle fix is not exercised by any comparison here (no Auto-Tune Artist in the rehearsal set; derive-only over
  Sean's zip does not reach it).
- bx_digital's Input / Output Gain still read as an unnamed "EQ band" in the live run although the grid guard exists (in the
  appendix as open).
- Timing and multiband do not probe unnamed controls (said on every record).
