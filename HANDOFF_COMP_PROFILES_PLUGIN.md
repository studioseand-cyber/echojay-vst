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

`docs/COMP_PROFILE_SPEC_v1.md` is the spec, copied verbatim from `~/Desktop/COMP_PROFILE_SPEC_v1.md`. **It is now
v1.4** (the file's own status line reads `DRAFT v1.2`; v1.1 brought the full 64-hex `map_fp`,
`measured.reference_ratio` and `controls_norm`, and v1.2 is Kathy's review — measured GR points replacing the
threshold formula, 997 Hz, the RMS convention pinned, stepped controls, `detector`, tighter acceptance).

### What v1.1 / v1.2 changed on this side — all four done, RED first

| # | change | where |
|---|---|---|
| 1 | a full-scale **997 Hz** sine must read `loud_rms_dbfs` **−3.01** (plain RMS, not AES17) | `EJTrackLevel.h` |
| 2 | **`controls_norm`** entries are written as raw 0..1 norms | `EchoJayParamApply.h` |
| 3 | the profile lookup compares the **full 64-hex** `map_fp`, not the 12-char log form | `ChainHost.cpp` |
| 4 | on a **stepped** amount control the correction moves to the adjacent listed **detent** | `EJCompCheck.h` |

**1 — the convention.** It was already plain RMS, but it reported the histogram **bin's lower edge**, 0.25 dB wide,
which answers `-3.00` where the spec's test says `-3.01`. The p95 bin now carries the **mean of the dB values that
fell in it** (one `double` per bin, no extra work on the audio thread), so the figure is the real level; and the
wire carries **two decimals**, because one cannot express `-3.01`. Verified on a full-scale 997 Hz sine:
`-3.010 dBFS` RMS and `0.00 dBFS` peak, so the pair differ by exactly the sine's 3.01.

**2 — `controls_norm`.** A sibling map on the block, deliberately **not** routed through `applyOne`: that resolves a
value against a map entry's positions and units, and the amount position the server picks off `amount.curve` is a
norm with no display text to resolve against — the whole point of the curve is that it was measured at norms. So
these are written straight to the parameter, clamped to 0..1, and **counted as applied** in the report with the norm
they wrote. A non-number writes nothing and says so; a control the plugin does not have is named. The settle
verifies the **norm** (`|value − normalized| ≤ 0.02`, which is exactly the right test), and it needed `index` and
the *pre-write* `settlePrevNorm` to do that — without them a correct write would have been reverted as "parameter
vanished".

**3 — the full fingerprint.** A profile whose `map_fp` is not this slot's full 64-hex fingerprint is refused and
treated as no profile, because it was measured on another binary. Compared in full, logged short — and when the
claimed value is a *prefix* of the slot's, the log says so outright: "that looks like the 12-char LOG form, which is
not the key".

**4 — stepped detents.** `amount.stepped: true` → the correction moves to the **adjacent listed detent** in the
direction of less gain reduction, never an interpolated norm, because a norm between two detents lands on whichever
one the plugin rounds to — a position nobody chose. Adjacent rather than nearest-to-ideal, so one correction stays
one move even when the ideal lies past the next detent; at the last detent it stays put. A continuous control still
interpolates exactly.

`track_level_guard` 25 assertions GREEN · `comp_profile_guard` 50 assertions GREEN.

### v1.3 (Kathy's second review) — two changes landed

**`loud_peak_dbfs` is now defined** (§5): "over the same 400 ms windows, the maximum absolute sample value in each
window (no oversampling), then the 95th percentile of those across what was heard." It is its **own percentile over
its own distribution** — one max-|sample| figure per window, over the same windows the RMS percentile uses — and not,
as v1.2 left to the reader, the largest peak among the loud windows.

**The two definitions give different answers, and that is the point.** 60 s of −12 dBFS sine phrases with **one
full-scale sample** in one window of 150: the old reading was the largest peak among the loud windows, so the click
set it and it read **0.0 dBFS**; the percentile discards the top 5% of windows, so it now reads **−8.99 dBFS** — the
material. With no outlier the two agree, so the change is about the outlier and nothing else.

**`eff_threshold_dbfs` became optional and informational** — "The server never reads it; `in_at_gr_dbfs` is the only
threshold field it uses" — and the v1.3 example omits it from **every** curve point. **You did not ask me to touch
this, and I did**, because `CompCheck::curveOf` read only that field: on a v1.3 profile the curve would come out
**empty**, `amountNormForLessGr` would return the current norm, and yesterday's one correction would have **silently
done nothing** while reporting success. That is worse than a wrong move. The curve now reads
`in_at_gr_dbfs["1"]` first and falls back to `eff_threshold_dbfs`, so v1.2 and v1.3 profiles both work; a point
whose `in_at_gr_dbfs["1"]` is `null` is left out, because the sweep never reached 1 dB there and it cannot anchor
anything. Five assertions, including the empty-curve RED.

### v1.4 — the server is the single gate, and the tone check exists

**The plugin's own profile trust check is GONE.** v1.4 names `quality.point_error_db` as *the* trust gate and it is
the server's: it only attaches a `comp_profile` that passed its validator, so **the plugin uses any `comp_profile` it
receives**. What was removed: the `fit.max_error_db > 1.5` gate, the schema check and the topology check. Two copies
of a rule is one too many — they drift, and **v1.3 retiring `fit` for `quality` is exactly how that drift showed
up**: the plugin was still enforcing a field the contract had already replaced.

`readCompProfile` now *reads* rather than *judges*: `usable` means only "this is an object I can read", and the
fields are parsed for the log and the closing line. Both quality figures are logged when present (v1.2's
`fit.max_error_db`, v1.3+'s `quality.point_error_db`) so a profile's own number is visible beside what it did —
logged, never acted on. A profile with no amount control is used too: it names the compressor in the line, there is
simply nothing to correct *with*, and that is reported rather than hidden.

**One refusal I kept, and it is a judgement call.** A profile whose `map_fp` is not this slot's full 64-hex
fingerprint is still refused. That is not "is this profile good" — which is the server's call — but "is this profile
**for this slot**", and using one measured on another binary would dial a threshold from somewhere else. You asked
for that check explicitly when v1.1 landed and have not withdrawn it. Say the word if the server owns that too.

**`comp_render_check --tone <dBFS>`** (spec §8): renders a **997 Hz** sine at that RMS through the given settings and
reports GR under its own `tone_check` key, with the 0.5 dB tolerance printed beside it. 997 Hz, not 1000 — §4 puts it
"off the 1000 Hz default crossover some multiband compressors use". It opens with a 1 s fade so a compressor's attack
is not measured against a step, and a level above a full-scale sine's −3.01 dBFS RMS is clamped and **said so**
(`clamped_to_full_scale`) rather than silently clipped.

Proven in both directions on `AUDynamicsProcessor`:

```
--tone -10  --set "Compression Threshold=-20"   →  loud_rms_in_dbfs -10.00,  gr_db 12.94
--tone -30  --set "Compression Threshold=-10"   →  loud_rms_in_dbfs -30.00,  gr_db  0.00
```

The tone renders at **exactly** the requested RMS, which is the part the tool controls, and with the threshold 20 dB
above the tone the measurement reads **0.00** — so a GR figure from it means compression and not an artefact. I am
**not** claiming the 12.94 matches a predicted ratio: `AUDynamicsProcessor` exposes no compression ratio (its
parameters are Compression Threshold, Headroom, Expansion Ratio, Expansion Threshold, Attack, Release, Master Gain),
so there is nothing to predict against. The number to trust is the zero.

### Still owed from v1.2, v1.3 and v1.4, NOT done

- **§6's new computation is server-side** (measured `in_at_gr_dbfs` points replacing the threshold formula). Nothing
  owed here beyond what is already sent, but note the plugin now sends `loud_peak_dbfs` to two decimals, which is
  what `detector: "peak"` needs (`loud_peak_dbfs - 3.01`).
- **`detector_f` is REQUIRED in v1.4** and feeds `L = loud_rms + f x (loud_peak - loud_rms - 3.01)`. That
  computation is the server's, and the plugin already sends both figures it needs to two decimals. Nothing owed here
  — noted only so nobody looks for it on this side.
- **§11 version matching**: profiles key to the plugin version they were swept on. Waves on the EJ Maps Mac is V12;
  this machine runs 15.0.70, so a V12 profile will not match. Nothing to build — it is a sweeping instruction — but
  it is the likeliest reason a first real profile fails to join.

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
./build-guards/guards/comp_render_check --id "AudioUnit:Effects/aufx,dcmp,appl" \
    --set "Compression Threshold=-30" --set "Headroom=2" --seconds 60 --json /tmp/out.json
```

> **IT NEVER SCANS, AND IT REFUSES LICENCE-BOUND PLUGINS.** `--id` names one AudioComponent and is required;
> `--list` is gone. See "THE HARM THIS TOOL DID" below — this is not a preference, it is the rule.

- GR pairs 400 ms windows of the render against the **same** windows of the input and takes those at or above the
  input's 95th percentile — the same statistic the plugin sends as `track_level`.
- **Static gain is measured, not assumed:** the same signal 40 dB down is well below any threshold, so
  output-minus-input there is the fixed offset, and it is subtracted.
- A control is matched against the parameter's own **panel text** first (so `On` and `4.00` land exactly), then by
  number, then as `norm:0.42`. Every landed text is reported, so a value that did not take is visible.
### THE HARM THIS TOOL DID, and what now prevents it

The first version resolved a plugin NAME by scanning. Resolving a name means asking every AudioComponent on the
machine what it contains, and for licence-bound plugins that means **loading** them. On Sean's Mac that drove
**iLok/PACE authorisation prompts and crashes**, with his iLok on another machine, and the process was killed twice
(exit 144). That is real harm done to a working machine by a tool of mine, and no measurement was worth it.

Ruled 1 Oct 2026, and now enforced in the code rather than remembered:

- **There is no scan and no `--list`.** `--id` is **required** and names exactly one AudioComponent; `findAllTypesForFile`
  is called on that id alone and no search path is ever walked. Without `--id` the tool prints usage and exits.
- **Licence-bound ids are REFUSED before anything is loaded** — Waves (`ksWV`), UAD (`uadx`), and the other PACE/iLok
  codes. Verified: `--id "AudioUnit:Effects/aufx,EMO5,ksWV"` and `--id "AudioUnit:Effects/aufx,1176,uadx"` both
  return `{"error": "refusing a licence-bound plugin on this machine", ...}` and load nothing.
- **Those plugins belong to EJ Maps**, which has to solve the same licence wall anyway (spec §4, "licence-bound
  plugins fail out of process"). They are not to be loaded on this Mac.

The Apple path still works through `--id`, re-verified after the change: `AudioUnit:Effects/aufx,dcmp,appl` gives
the same 12.03 dB.

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

**EMO-D5 IS NOT, AND WILL NOT BE, MEASURED ON THIS MACHINE.** It is a Waves plugin behind WaveShell, which is
PACE-wrapped. An unsigned binary cannot load it, the attempt prompts for an iLok that lives on another Mac, and the
prompt takes the host down. The tool now refuses it outright. **It goes to EJ Maps**, which owns licence-bound
plugins by the spec. If a measurement on this Mac is ever genuinely needed, it needs Sean's decision and a signed
binary with `com.apple.security.cs.allow-unsigned-executable-memory` - not a workaround.

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
