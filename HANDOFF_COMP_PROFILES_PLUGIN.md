# HANDOFF — measured compressor profiles, plugin side

1 Oct 2026, unattended day session. Branch `feat/comp-profiles` off the Part 1 commit. Nothing pushed, nothing
installed, nothing signed. `~/echojay-saas` untouched.

---

## PART 1 — last night's gate: NOT GREEN, so NOT PUSHED

The fast gate was run twice. The honest result is that it is **not green**, and the reason it is not green is
**not yet attributed**. Commits are local on `merge/kathy-2026-09-06`; nothing was pushed.

### What is green

| guard | result |
|---|---|
| `level_loop_guard` | GREEN, 0 of ~180 assertions failed |
| `loudness_loop_guard` | GREEN — 17 legs marked SUPERSEDED (see below) |
| `level_slot_guard` | GREEN — was a real defect, fixed (see below) |
| `ui_guard` | GREEN |
| `calib_link_guard` | GREEN, both sides |
| `track_level_guard` (new) | GREEN, 16 assertions |
| `comp_profile_guard` (new) | GREEN, 31 assertions |

### What is not, in the main tree

```
level_loop_guard      (green when run directly; failed only in a gate run whose build had aborted)
linkmixer_test
builtin_registry_test
probe_plist_guard
calib_link_guard      (green when run directly)
alias_mirror_guard
lease_id_guard
level_match_guard
role_snapshot_guard
```

### The 17 SUPERSEDED legs in `loudness_loop_guard`

Each carries one line naming the rule that replaced it, and `supersededCheck` prints `SKIP SUPERSEDED` and never
counts as a pass. The original assertion text is kept verbatim so the record of what *was* true survives. Three
causes:

- **(q)** — a compressor build writes no drive, so fixtures that relied on the seek pushing a slot's input above
  −3 dBTP now read `-200.00 dBTP`, and the Listen pictures, peaks and band claims they assert cannot exist.
- **(g)** — a loop ends at its close, so a live card or a window after the close is unreachable.
- **21t-m item 1** — two legs still assert the compare-only post trim that was deleted.

### `level_slot_guard` R2g — a real defect, and it was ours

The leg reads "four value writes move the VALUE counter" and got `2 -> 5`. `setSlotPreTrimDb`, `setSlotWet` and
`setMasterWet` all call `bumpChainValue()`; **`setSlotOutGainDb` did not.** Cause: 21t-m item 1 deleted the
compare-only trim and moved the slot's OUT into its place everywhere *except* that counter — so the one control the
**hold** writes was the one value write the counter could not see. Fixed by adding the bump.

It fails identically at `66a775b`, so it predates letters (l)–(q). `getChainValueRevision` has **no production
consumer** today (only the guard reads it), so nothing was losing a saved value; the next consumer would have
inherited a counter that lied about OUT.

### CORRECTION — the four "pre-existing reds" are NOT attributed

Earlier in the session I reported that `alias_mirror_guard`, `lease_id_guard`, `level_match_guard` and
`role_snapshot_guard` were "already red at the baseline, not ours". **That was wrong and I am withdrawing it.**
All four baseline runs failed with:

```
FAIL  the Link archive does not exist at build-release/EchoJayLink_artefacts/Release/libEchoJay Link_SharedCode.a
FAIL  link side does not compile on this tree - RED by construction
```

That is my own staleness refusal doing its job in a worktree that has no `build-release` artefacts — an
**environmental** refusal, not a verdict on the code. The same wall voided five of the nine failures in the
worktree gate (`probe_plist_guard` wants a built probe; every Link-side guard wants the Link archive).

**To attribute them properly**, in the baseline worktree first:

```
cmake --build build-release -j 4 --target EchoJayLink EchoJayProbe
```

then run each guard's `build_and_run.sh`. The only attribution that stands today is `level_slot_guard`, which is a
CMake target, built and ran at the baseline, and failed there with the identical assertion.

### DECISION taken on Part 1

The 90-minute cap passed with the gate unresolved, so by the brief I left Part 1 committed locally and moved to
Part 2. I also **stopped a running gate** and re-ran it in a clean worktree at the Part 1 commit, because Part 2
edits the same headers (`EJCalibLoop.h`, `ChainHost.h`) and a gate cannot mean anything while they are changing.
That worktree is at `scratchpad/p1gate` and can be removed with `git worktree remove`.

---

## PART 2 — the plugin side of measured compressor profiles

`docs/COMP_PROFILE_SPEC_v1.md` is the spec, copied verbatim from `~/Desktop/COMP_PROFILE_SPEC_v1.md`.

### The flag

```
touch  ~/Library/EchoJay/comp_profiles_on.txt     # on
rm     ~/Library/EchoJay/comp_profiles_on.txt     # off  (the default)
```

Read fresh at each use — no relaunch to switch, and a day's testing can turn it off the moment it misbehaves.
**With the flag off, behaviour is exactly letter (q):** a compressor build writes no IN, the hold matches the level
on OUT once, and the line says "set as dialled". `comp_profile_guard` asserts the flag is off by default.

### Item 1 — `track_level` on the build request (spec §5) — DONE

`Source/EJTrackLevel.h`, `echojay::TrackLevel`. The 95th percentile of 400 ms RMS over what has been **heard**,
measured **pre-chain** on the raw input before the pre-chain gain, plus the loud-phrase peak. Sent on every build
request as `track_level`; under 20 s heard the field is **absent**, which is how this body expresses the spec's
null (a `0` would be read as a level).

- **Why a percentile:** the server subtracts this from the profile's `eff_threshold_dbfs`, so what it needs is the
  level of the material that is *supposed* to be compressed — not the mean of a take that is mostly silence, and
  not the single loudest 400 ms, which is one breath from being an outlier.
- **Why a histogram:** a percentile needs the distribution and this runs on the audio thread. 385 counters at
  0.25 dB from −96 dBFS — no allocation, no sorting, exact to a quarter of a dB, well inside the 1.0 dB §8 allows.
- **Units:** plain RMS dBFS. That is what `level_ref: "sine_rms_dbfs"` means — the sweep reports its sine's RMS in
  dBFS and this reports the programme's the same way, so §6's subtraction is between like and like.

`tools/track_level_guard` — GREEN, 16 assertions on synthetic signals whose levels the guard chose: the percentile
is the loud level to within half a dB at 25% *and* at 10% loud, digital black is heard for 0 s, material under the
gate does not buy the 20 s, 4 s is ten windows, and the wire object is the spec's four fields.

### Item 2 — the profile and the expectations on the slot — DONE

`ChainHost::slotCompProfile(slot)` reads `comp_profile` live out of the parameter-map payload under the slot's own
map fingerprint, which is the join key §2 names. `expected_gr_db` / `expected_level_db` are read off each chain
block (as `wet_pct` is) and stored on the slot, **NaN when the block says nothing** — because 0 dB of expected gain
reduction is a different statement from "the block said nothing".

A profile that cannot be trusted is reported **absent** rather than half-used, with the reason logged once: schema
other than `ej_comp_profile/1`, topology `other`, no amount control, or `fit.max_error_db` over 1.5 dB. A built-in
carries no fingerprint, so it can never have a profile — correct, since profiles exist for third-party compressors.

### Item 3 — the one check (spec §7) — DONE

`Source/EJCompCheck.h`, `echojay::CompCheck` — pure and header-only, so a guard drives all three outcomes with
figures of its own and the V2 and the Link cannot decide differently. After the dial settles and 10 s of loud
material at that slot:

- **more than expected + 3 dB** → the amount control moves **once**, by the difference, toward less gain reduction.
  Its target is read off the profile's own `amount.curve` from where the control **actually is** (read back off the
  plugin, not believed). No second move.
- **under 0.5 dB when expected is ≥ 1 dB** → `PROFILE_NOT_ENGAGING` with plugin, map_fp and the readings. Nothing
  moves.
- otherwise nothing. Then the OUT hold, once.

**A sign error worth knowing about.** `static_gain_db` and `expected_level_db` come off the **change**, and the drop
is the negation of what is left:

```
compressionChange = levelChange - static_gain - expected_level
drop              = -compressionChange
```

Doing it on the drop flips both corrections. My first cut did, and **a leg asserting the wrong number agreed with
it** — it passed `-2.0` while its comment said `+2`. Both are fixed and the comment names the worked examples.

### Item 4 — the closing line per compressor — DONE

`EMO-D5: about 2 dB on the loud phrases, from its profile. Output -1.5 dB.` and
`NEOLD U2A: set as dialled, no profile yet.` The plugin's name leads, because on a two-compressor build the user
needs to know which one it is about. With a correction: `I eased it back 4.5 dB.` Not engaging:
`It is not compressing at all - the profile looks wrong and I have reported it.`

`tools/comp_profile_guard` — GREEN, 31 assertions: the flag default, publish/read by fp on a real fingerprinted
plugin, all four untrusted-profile cases, `input_drive` accepted, expectations stored and cleared, a built-in has
none, the three outcomes, the boundary at exactly expected + 3 (which must **not** move), the amount moving up the
curve by exactly the dB asked for, the `PROFILE_NOT_ENGAGING` fields, the expected-under-1-dB floor, both gates and
both subtractions.

### Item 5 — `tools/comp_render_check` — BUILT, NOT YET PROVEN ON EMO-D5

The §8 acceptance check. Loads a real AU in process, applies `NAME=VALUE` controls, renders audio through it, and
reports GR on the loud phrases as JSON. **Not** a profiling sweep — that is EJ Maps' job — and it carries ctest
label `none`, so no gate runs it.

```
./build-guards/guards/comp_render_check --file WaveShell --name "EMO-D5 (s)" \
    --set "Comp=On" --set "Comp Thresh=-20" --set "Comp Ratio=4" --json /tmp/emo.json
```

- GR pairs 400 ms windows of the render against the **same** windows of the input and takes those at or above the
  input's 95th percentile — the same statistic the plugin sends as `track_level`.
- **Static gain is measured, not assumed:** the same signal 40 dB down is well below any threshold, so
  output-minus-input there is the fixed offset, and it is subtracted.
- A control is matched against the parameter's own **panel text** first (so `On` and `4.00` land exactly), then by
  number, then as `norm:0.42`. Every landed text is reported, so a value that did not take is visible.
- **`--file` narrows the scan to one component, and it matters:** scanning every AU on this machine walks UAD's and
  PACE's component registration, which **killed the tool outright** the first time it ran. A Waves plugin lives
  inside WaveShell, so `--file WaveShell` reaches EMO-D5 without loading anything else.

**THE SIGNAL: generated, not a vocal clip.** The repo has **no** `.wav` assets at all (`refs/vocal_ref_01.wav` in
§8 is still "to be chosen by Sean"), so the tool generates a speech-like signal: a ~160 Hz glottal pulse train
through a formant-ish tilt, amplitude-modulated into 1.4 s phrases with 0.5 s gaps, normalised so the loud phrases
sit at −18 dBFS RMS. It is **not a voice**; it is a repeatable envelope with known levels, which is what an
acceptance number needs to be comparable between runs. `--wav <file>` takes a real clip the moment one exists, and
that is the thing to do before any profile is accepted.

**PROVEN END TO END, on a non-PACE compressor.** Apple's `AUDynamicsProcessor`, 60 s of the generated signal:

```
./build-guards/guards/comp_render_check --file ",appl" --name "AUDynamicsProcessor" \
    --set "Compression Threshold=-30" --set "Headroom=2" --seconds 60

  "controls": [ { "control": "Compression Threshold", "asked": "-30", "landed": "-29.8", "norm": 0.5850, "ok": true },
                { "control": "Headroom",              "asked": "2",   "landed": "2.095", "norm": 0.0500, "ok": true } ],
  "loud_windows": 11,
  "loud_rms_in_dbfs": -18.21,
  "loud_rms_out_dbfs": -30.23,
  "static_gain_db": 0.00,
  "gr_loud_db": 12.03
```

Both controls landed and were read back; 12 dB of gain reduction from a threshold 12 dB under the material is
physically sensible. The control-name matcher earns its keep here: asking for `Threshold` reported
`(no such control; this plugin has: Compression Threshold, Headroom, Expansion Ratio, Expansion Threshold, Attack
Time, Release Time, Master Gain)` rather than silently doing nothing — and the profile's control names have to match
the plugin's exactly (§3), so that list is the useful half of the answer.

**ONE THING TO KNOW ABOUT CLIP LENGTH:** the loud set is the top 5% of 400 ms windows, so a 12 s clip gives **2**
windows and a 60 s clip gives **11**. Use 60 s or more for any number you intend to accept a profile on.

**STUCK on EMO-D5 specifically, documented and moved on (the brief's rule).** I have not got a measured GR off
EMO-D5. EMO-D5 is a
Waves plugin behind WaveShell, which is **PACE-wrapped**, and an unsigned binary cannot load it — the known wall
from the 21 Sep scan (12 PACE SIGKILLs; the probe needed
`com.apple.security.cs.allow-unsigned-executable-memory`). Giving this tool that entitlement means codesigning it,
which I was told not to do. **Two ways forward, both yours to pick:** sign `comp_render_check` with that
entitlement, or run it against a non-PACE compressor first (Apple's `AUDynamicsProcessor`, or EchoJay's own
compressor) to prove the measurement end to end, and treat Waves plugins as EJ Maps' problem since EJ Maps has to
solve the same licence wall anyway (spec §4, "licence-bound plugins fail out of process").

---

## COMMITS (all local, branch `feat/comp-profiles`)

| commit | what |
|---|---|
| `ba61a36` | letters (l)–(q): the loop rules, the two 19:02 faults, the compressor seek withdrawn |
| `7c348a8` | the value counter sees the hold's OUT; 17 legs marked SUPERSEDED; `CalibLoop::endedAs` |
| `6368582` | **the Part 1 commit** — `docs/COMP_PROFILE_SPEC_v1.md` |
| `5c95080` | comp profiles items 1 and 2 |
| `b67a5a8` | comp profiles items 3, 4 and 5 |

`merge/kathy-2026-09-06` ends at `6368582`. `feat/comp-profiles` branches from it.

## THE PLACED BUILD

**Path:** `~/Desktop/ej_dev_2026-09-21t-m/EchoJay V2.component`
**LC_UUID:** `C9D6E011-2F05-33D4-A4E0-BAF6797EBA8A`  (1 Oct 09:58, 67M, verified against the build tree)

V2 AU only, and the flag is **off**, so installing it behaves exactly as (q). The one visible difference with the
flag off is that every build request now carries `track_level`, which the current server ignores. The Link bundle in
that folder is still **(f)** and unchanged. Drop-in:

```
cp -R ~/Desktop/ej_dev_2026-09-21t-m/"EchoJay V2.component" ~/Library/Audio/Plug-Ins/Components/
```

Kill `AUHostingService` afterwards or Logic keeps the old binary alive.

## WAITING ON

**The server** (`echojay-saas` branch `feat/comp-profiles`, B): compute the settings from the profile and
`track_level` per §6; send `engage` + `neutral` + amount + ratio as ordinary `settings_structured.controls`; send
`expected_gr_db` and, for `input_drive`, `expected_level_db` with each block. The plugin reads all of that already.

**Kathy / EJ Maps:** one real profile JSON to test against — §10's five questions are still open, and the join key
(`map_fp`) is the one that decides whether anything lines up. For reference, the map fingerprint this machine
computes for **EMO-D5 (s)** is **`32b7e1d9a0c3`** (uid `4942687d`, AudioUnit, version 15.0.70, 59 params), which is
the same value the spec's own example prints — so the example was taken from a real map and the join key works.

**Sean:** `refs/vocal_ref_01.wav`, the reference clip §8 needs; and whether `comp_render_check` may be signed with
`allow-unsigned-executable-memory` so it can load PACE-wrapped plugins.

## DECISIONS taken unattended

1. **Part 1 was not pushed.** The gate is not green and the failures are not attributed. The brief said push only
   if green.
2. **I stopped a running gate and re-ran it in a clean worktree** at the Part 1 commit, because Part 2 edits the
   same headers and a gate cannot mean anything while they change. A worktree turned out not to be equivalent —
   see the CORRECTION above — which is itself the finding.
3. **`sine_rms_dbfs` read as plain RMS dBFS.** The sweep reports a sine's RMS in dBFS; `track_level` reports the
   programme's RMS in dBFS. Same reference, so §6's subtraction is meaningful. A full-scale sine reads −3.01.
4. **`loud_peak_dbfs` is the peak of the loud windows**, not of the whole take: a single click in a quiet bar is
   not a loud phrase.
5. **A silence gate of −70 dBFS on a 400 ms window**, and material under it counts neither as heard nor in the
   statistic — so a long quiet head cannot buy the 20 s.
6. **A profile that cannot be trusted is reported absent**, not half-used, so every caller takes the no-profile
   road rather than each re-deciding. The reason is logged once, where the decision is made.
7. **`input_drive` is accepted as well as `threshold`**; `other` is treated as no profile, per §3.
8. **One rule serves both topologies for the correction:** less gain reduction is a *higher* effective threshold,
   whether the amount control is a threshold or an input/peak-reduction knob.
9. **The amount's current position is read back off the plugin** when the loop starts, not assumed to be where the
   server put it, so the correction is measured from where the control actually is.
10. **A companion hold gets the profile treatment too** — the stamp runs for every loop a build starts, not only
    the primary.
11. **`comp_render_check` is a tool, not a test**: ctest label `none`. It loads real AUs and renders audio, so a
    gate must never run it.
12. **The test signal is generated and the handoff says so**, because the repo has no `.wav` assets.
13. **`--file` was added to narrow the scan** after a full AU scan killed the process on UAD/PACE registration.
