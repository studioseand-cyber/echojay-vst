# HANDOFF — measured compressor profiles, plugin side

1 Oct 2026, unattended day session. Branch `feat/comp-profiles` off the Part 1 commit. Nothing pushed, nothing
installed, nothing signed. `~/echojay-saas` untouched.


---

## Overnight, 1-2 Oct 2026 — the gate, the real server output, and the preview switch

Read this first; it supersedes nothing below it, it only adds.

### DECISIONS taken without Sean (he was asleep and said to take the most reasonable reading)

1. **The `window` label was OUR bug, not B's.** The plugin sent `"400ms_rms_p95"`; the contract
   (`docs/COMP_PROFILE_SPEC_v1.md` v1.4, section 5, line 124) prints `"400ms_p95"`, and B's generated request
   sends exactly that. The v1 label named the window after the RMS figure; that stopped being true at v1.3, when
   `loud_peak_dbfs` was defined over **the same** windows. Fixed plugin-side.
   **This was found by a guard that had already looked at it and let it pass** — see decision 2.

2. **A `check (true, ...)` is not a check, and this one cost a day.** The contract guard's window comparison
   printed a *note* saying "MISMATCH for B, not a plugin fault", argued from a premise ("the spec prints exactly
   that") the spec contradicted, and on agreement called `check (true, ...)`, which cannot fail. So the guard
   turned a plugin bug into a complaint about the server. It now asserts the plugin's label against the spec
   string, the server's label against the same string, and the two against each other — whichever side drifts is
   named, and neither gets the benefit of the doubt. (Same rule as the verifier one: a guard must be able to fail.)

3. **No plugin-side `detector_f` rule, deliberately.** Spec v1.4 makes `detector_f` required and says a profile
   without it is not used — but that is the *server's* gate: section 6 makes `f` the server's own threshold input
   (`L = loud_rms + f x (loud_peak - loud_rms - 3.01)`), and the plugin never reads it. Re-checking it here would
   be the second copy of one rule, which is what the 1 Oct ruling removed. Sean's ruling kept the `map_fp` join
   because that is *identity* ("is this profile for this slot"), not quality — `detector_f` is neither.
   A leg now pins the live trap instead: **B's real EMO-D5 profile has `detector_f: 0`**, and 0 is pure-RMS
   detection, not a missing field. Anyone adding that rule as a truthiness test would reject every pure-RMS
   compressor in the catalogue while looking entirely reasonable in review.

4. **The gate's own two faults were fixed before the gate was believed** (commit `9d47209`, on
   `merge/kathy-2026-09-06`, because they are gate infrastructure and both files were byte-identical on the two
   branches). `level_loop_guard` case (5) asserted that a second tick was "inside the 3 s window" — true only if
   the machine feeds 3 s of audio in under 3 s of wall clock, which it did not under 42 guards with malloc
   hardening. It now **measures** that interval and prints NOT TESTED with the number rather than failing, because
   a leg whose premise the machine can take away is not a test of the product. And `run_guard.sh` printed a 40-line
   tail on a scribble failure — forty lines of `ok`, which proved only that *something* failed; it now greps every
   FAIL line first.

### Item 2 — the contract test against B's real output

B wrote `~/Desktop/ej_contract/` at 21:32 (`map_payload_emo.json`, `build_request.json`, `build_reply.json`,
`README.md`), generated from the real server code path at `beaa5dc7` on `feat/comp-profiles`.

`tools/comp_contract_guard` feeds **those exact files** through the plugin's real functions — `readCompProfile`,
`CompCheck::curveOf`, `configsFromBlock`, `applyStructuredSettings`, `TrackLevel`, `completedLine()` — not
through a fixture written to match.

**Checked and correct, no change needed:**

- `in_at_gr_dbfs` is `null` at every GR for `norm: 1.0` (the sweep tops out at -3.01 RMS). `curveOf` type-checks
  before reading and skips those, leaving 10 usable anchors. A `(float)` cast would have read them as 0 dBFS and
  bent the top of the curve.
- The calibration block carries **explicit JSON `null`** for `param`, `start_db`, `sense`, `min_db`, `max_db`,
  `heard_s` and `measure`. Every one of those reads goes through an `isVoid()` + type check, so null is "unset",
  never zero — the 21t-g ruling, holding on real data.
- Slot numbering: B sends 1-based (`slot: 2` is the EMO, chain index 1). Both parse sites convert correctly.
- `mapFps` carries the **full 64-hex** fingerprint, and `paramMaps_` is keyed the same way, so the join matches.
  (`buildMapFpsJson` never truncates; the 12-char form is only ever a log form.)
- The unprofiled NEOLD U2A slot carries no `controls_norm`, no `expected_gr_db` and no `from_profile`, and the
  plugin invents none of them — its closing line says "no profile yet".

**Nothing was found that is the server's fault.** `~/echojay-saas` was not touched.

5. **The stale label was in FOUR places, not one.** Fixing the wire value alone would have left the repo
   teaching the old string. Swept and fixed: `Source/EJTrackLevel.h` (the wire value **and** the doc example),
   `tools/track_level_guard/harness.cpp` (its header example **and** its assertion), and
   `tools/comp_render_check/main.cpp:379`, which **emits** `window` into the section 8 acceptance JSON — a real
   output, not a comment. An acceptance tool reporting a non-conforming label would have been the next day's
   confusion.

6. **`track_level_guard` agreed with the bug.** Its leg asserted `"400ms_rms_p95"` — the same stale string the
   product used, because leg and code were written from one misreading. It could never have caught this; the only
   thing that disagreed was the server's own generated output. That is the argument for item 2's whole approach:
   a test fed by the other side's real bytes finds what a test written beside the code cannot.

7. **Red 1 of the gate was NOT fixed tonight, deliberately.** It is root-caused and the fix is specified, but it
   changes a shared header (`EJCalibLoop.h`), which by this repo's own rule means rebuilding every guard and
   re-running the suite — about an hour. The push is blocked by reds 3-5 regardless of red 1, so that hour buys
   nothing tonight and was spent on items 2-4, which were asked for. First thing for the morning.

8. **`build-guards` was still generated from the OTHER branch**, and this nearly produced a false green. After
   checking out `feat/comp-profiles` the first guard run reported `BUILD=2 errors=0` and `CTEST=0` — which looks
   like a pass if you read the exit code. In fact the comp guards did not exist as targets (`No rule to make
   target 'track_level_guard'`) and ctest matched nothing (`No tests were found!!!`). A CMake tree carries the
   branch it was configured from; after any checkout it must be regenerated (`cmake -S . -B build-guards`) before
   its results mean anything. Same family as the archive-staleness ordering in item 1.

9. **A third memory-shaped finding, and a hypothesis I am NOT yet asserting.** Three independent failures tonight
   all have the shape of a `juce::String`/`var` standing on memory it does not own:

   | Where | What it does |
   |---|---|
   | `v2_side.cpp:67` (reds 3-5) | `EXC_BAD_ACCESS at 0x800` comparing `li.uid` - a `LinkSlotInfo.uid` with a dangling character pointer, inside `linkSlotInfos` |
   | `level_loop_guard` (6a), scribble only | Apple's AUDelay counts as a **dynamics** slot when its own category is a delay - a `paramMaps_`/category read |
   | `comp_profile_guard` (2a), scribble only | with **no** profile published, `slotCompProfile(0)` returns non-void - a `paramMaps_` lookup finding something that was never stored |

   Both scribble failures are `paramMaps_` lookups keyed by fingerprint, and both only appear when freed memory is
   poisoned, which is what MallocScribble does. That is suggestive of the map store retaining something it does not
   own - the same class as the 18 Sep Pro Tools crash and the rule this repo already has about it. **It is a
   hypothesis, not a finding**: I have not traced either one to a specific owner, and three failures sharing a
   smell is not the same as three failures sharing a cause. Worth one focused session with ASan rather than more
   guessing. The ASan run already owed for the latency-rebuild use-after-free may well answer all three at once.

   Neither scribble failure can be caused by tonight's commits: `comp_profile_guard` and the map paths were not
   touched, and the only product change is a string literal in `EJTrackLevel.h`.

### Item 1 — the Part 1 gate: NOT PUSHED, and exactly why

Run in the **main tree** (not a worktree) at `9d47209` on `merge/kathy-2026-09-06`, with the release Link+Probe
archive rebuilt **after** the checkout — the ordering that caused five bogus staleness refusals last night.

```
GUARDBUILD=0  errors=0
FAST=8        91% tests passed, 5 tests failed out of 53     (22:24 -> 23:08)
```

**Per Sean's rule the branch was NOT pushed:** the reds are not all proven pre-existing. Two are ours and one is
still unexplained. The 88 commits stay local.

| # | Red | Whose | Status |
|---|---|---|---|
| 1 | `calib_link_guard` (f) settle budget | **OURS** | root-caused, fix specified below |
| 2 | `level_loop_guard` scribble leg (6a) | **OURS** (leg added by (l)-(q)) | reproduces serially, unexplained |
| 3 | `lease_id_guard` | unattributed | V2 side crashes; see below |
| 4 | `level_match_guard` | unattributed | same crash |
| 5 | `role_snapshot_guard` | unattributed | same crash |

All five reproduce with `-j 1`, so none of them is a concurrency flake. I checked that first and was wrong about
it; stating it plainly because "it's just load" would have been the convenient answer.

#### Red 1 — `calib_link_guard`: one rule, written twice, updated once

`FAIL (f) ...and its settle budget opens already SPENT, so it lands on the first judged window [0 of 3]`

`EJCalibLoop.h` derives the purpose's settle budget in **two** places:

- line 676, `begin(Config)`: `settleSteps = (purpose == buildHold) ? 0 : kSettleMaxSteps - 1`
- line 717, the 6-arg `begin(...)`: `settleSteps = (p == buildHold) ? kSettleMaxSteps : kSettleMaxSteps - 1`

Letter **(m)** changed the rule — a build now opens with its seek *ahead* of it, superseding (e)'s "already
spent" — and updated only `begin(Config)`. Line 717 still carries (e)'s value, and its comment claims it derives
"the SAME three things begin(Config) derives", which is no longer true. `PluginProcessor.cpp:6594` reaches that
overload with a caller-supplied purpose — the road the "Build this chain" pill takes — so **the same build gets
opposite settle budgets depending on which entry point it took.**

The assertion that caught it is *also* stale: it asserts (e)'s withdrawn rule. So this red needs both halves
fixed, and the fix is the one Sean has already applied twice to this file: the purpose-derived fields get derived
**once**, in one place, and the leg asserts that one rule.

#### Reds 3-5 — the V2 side segfaults ~100 ms into construction

Not a readiness timeout, which is what the surface output says. `link_ready.json` was written at 23:22:09 and the
V2 side started at 23:22:09.568, so the file was already there. The `the link side never became ready` line comes
from `harness_build.py`'s own dry run *before* the runner starts the Link side, and the `Segmentation fault: 11`
on the next line is the real V2 process. I misread that pairing at first.

Reproduced deterministically outside ctest, with no Link side and nothing installed, by pointing the existing
binary at an isolated home that still holds a `link_ready.json`:

```
HOME=$ISO ECHOJAY_STATE_HOME=$ISO EJ_STATE_TEST_HOME=$ISO EJ_LMG_HOME=$ISO/lmg \
  lldb -b -o run -o "bt 25" -- <scratch>/level_match_guard_v2_side_bin
```

```
EXC_BAD_ACCESS (code=1, address=0x8)   KERN_INVALID_ADDRESS
frame #0: main + 4028
->  ldr  x0, [x28, #0x8]        x28 = 0x0      x19 = 0x600000c50040
    cmp  x0, x19
    b.ne <back to +3996>
```

It walks a chain through the field at offset 8, comparing each link against `x19`, and never finds it: `x28`
reaches null and the next load faults. An unbounded walk with no null terminator check. Everything is inlined
into `main` in the release build, so the frame is not nameable from this binary.

**Last known-good run of these guards was 28 Sep**, whose `v2.log` continues past this point to
`EJScan: AU registry read, 1515 entr(ies)` and on to a GREEN verdict. Tonight's logs stop at
`EJScan: enabledState watch active`, six lines in. The regression window therefore includes `(l)`-`(q)`, which
touched `PluginProcessor.cpp` (+415) and `PluginEditor.cpp/.h` (+65/+154) — so these are **plausibly ours and
must not be assumed pre-existing**. I withdrew exactly this class of claim once before on these same four guards.

**Why it matters beyond the gate:** the crash is in constructing V2's processor and editor against a state root
with no entries cache — which is what a **first launch on a fresh machine** looks like. If that is what it is, it
is a user-facing first-run crash, not a harness artefact. That is the reason to chase it rather than exclude it.

#### A correction worth recording

While chasing this I concluded these guards were triggering a forbidden full AU scan, and said so. That was wrong
and I withdrew it: the `EJScan` path here reads AU **registry plist metadata** and VST3 folder listings and
writes a cache (28 Sep: 1515 AU + 912 VST3, complete in 0 s). It does **not instantiate plugins**, so it cannot
raise an iLok/PACE prompt. The operation Sean banned is `comp_render_check --list`, which really does load them.
No scan of that kind was run tonight, and nothing was installed.

**Named precisely, with a `-g -O0` rebuild of that one harness** (`EJ_CXXFLAGS="-g -O0" python3
tools/harness_build.py tools/level_match_guard/v2_side.cpp`):

```
EXC_BAD_ACCESS (code=1, address=0x800)
frame #0  juce::CharacterFunctions::compare<CharPointer_UTF8, CharPointer_UTF8>
frame #1  juce::operator== (juce::String const&, juce::String const&)
frame #2  main at v2_side.cpp:67:107        <- li.uid == m.uid
```

`mem.size()` is 3 and intact, so the corrupt `juce::String` is **inside `linkSlotInfos`**: a `LinkSlotInfo.uid`
whose character data pointer is garbage. Those are built at `PluginProcessor.cpp:6033-6056` from a registry
snapshot (`info.uid = snap.instanceUid`) and published by `linkSlotInfos = std::move(newInfos)`.

**What is left to do, and it is one sitting's work:** name what `snap.instanceUid`'s string data points into and
why it dies. The shape is the one this repo has a rule about already - a `juce::String` standing on memory it does
not own (the 18 Sep Pro Tools crash). Two concrete things to settle:
  1. whether it reproduces with a POPULATED state root as well as a fresh one (my repro used a fresh isolated
     home with a stale registry, which on its own could explain a dead Link's strings - but the gate run that
     failed had three LIVE Links, so the mechanism cannot only be staleness);
  2. whether `git stash`-ing `(l)`-`(q)`'s `PluginProcessor.cpp` changes makes it go away, which is the
     attribution answer Sean asked for and the thing I could not get tonight.

Until (2) is answered **these three reds are not attributed**, and the branch stays unpushed on their account
alone, never mind red 1.

### Item 3 — pointing the plugin at a preview server

An override already existed and was **useless for testing**: `~/.echojay/dev.json` (`baseUrl` +
`protectionBypass`) sits behind `ECHOJAY_DEV_TRANSPORT`, which is defined only for Debug or when the CMake option
is ON. Every build placed for Sean is Release with the option OFF — which is exactly why his logs read
`devTransport=off`. The switch could not be flipped in the binary he installs, so for his purposes it did not exist.

A file-based override is now compiled into **every** build, default off, read once per process:

```
# ON  — point at a preview deployment
echo "https://my-preview.vercel.app" > ~/Library/EchoJay/dev_base_url.txt

# ON  — only if that preview has Vercel deployment protection
echo "<bypass secret>" > ~/Library/EchoJay/dev_bypass.txt

# OFF — the default; the plugin talks to production exactly as before
rm ~/Library/EchoJay/dev_base_url.txt ~/Library/EchoJay/dev_bypass.txt
```

The path is `~/Library/EchoJay/` (JUCE's `userApplicationDataDirectory` is `~/Library` on macOS, not
`~/Library/Application Support` — verified against the flag files already living there).

- Read **once per process**, like `dev.json`, so set the file *before* launching the host.
- Absent, empty, commented out (`#`), or not `http(s)` => **no override at all**. A non-URL value is logged and
  ignored rather than used: `EJNet: dev_base_url.txt says "…", which is not an http(s) URL - IGNORED, talking to
  production`.
- The bypass **value is never logged** — only whether one was loaded.
- The startup line prints the whole base URL and where it came from:
  `EJNet: … base=https://… source=dev_base_url.txt` (or `source=built-in` on production).

### Item 4 — the build to install

**ONE build from `feat/comp-profiles` at `0482617`. Unsigned, NOT installed, nothing in `/Library`.**

```
/Users/SeanD/echojay-vst/ship_2026-10-02a/
  EchoJay V2.component     67M   LC_UUID 785D9918-99F4-3734-A637-158700FA7182
  EchoJay Link.component   49M   LC_UUID 625CD984-8CF9-3792-AF78-DFC69F2A6073
```

Both LC_UUIDs were read back from the placed bundles and match the build tree exactly — that is the install
proof, not the file dates, and not a `strings` grep (LTO folds literals and can false-negative).

The Link is in this build because the profile work lives in shared headers that both binaries compile. It
therefore also carries **(g)'s loop ending**, which Sean deferred to "the next Link build" — this is that build.
The Link has NOT been through the gate on this branch; only the three comp guards were run.

### Morning checklist

1. **Install** (V2 + Link, user plug-ins only — never `/Library`):
   ```
   PLACED=<path from item 4>
   rsync -a --delete "$PLACED/EchoJay V2.component"  ~/Library/Audio/Plug-Ins/Components/
   rsync -a --delete "$PLACED/EchoJay Link.component" ~/Library/Audio/Plug-Ins/Components/
   killall -9 AUHostingService 2>/dev/null; true
   ```
   The `AUHostingService` kill matters: without it the host keeps serving the previous binary and the log will
   describe code you are not running. Verify by the `LC_UUID` in item 4, not by the file dates.

2. **Turn the profiles flag on** (default OFF — with it off, everything below is dormant and behaviour is
   byte-for-byte what it is today):
   ```
   touch ~/Library/EchoJay/comp_profiles_on.txt     # on
   rm    ~/Library/EchoJay/comp_profiles_on.txt     # off
   ```
   Read fresh at each use, so it can go off mid-session the moment it misbehaves.

3. **Point at the preview** only if B has one up — see item 3 above. Set it before launching Logic.

4. **The log lines that show it working**, in the order they should appear:

   | Line | What it proves |
   |---|---|
   | `EJNet: … base=… source=dev_base_url.txt` | which server answered at all |
   | `EJCompProfile: slot N ("…") using the profile the server attached: schema=… topology=…` | the profile joined this slot by its full fingerprint |
   | `EJCompProfile: slot N … REFUSED a profile whose map_fp is not this slot's fingerprint` | the join check doing its job — if you see this, the profile is for another binary |
   | the `controls_norm` write lines for that slot | the amount control was set as a raw norm (it has no display text) |
   | the one-check outcome: in range / `PROFILE_NOT_ENGAGING` / one correction | section 7 ran on a real reading |
   | the closing line, e.g. `EMO-D5 (s): about 2 dB on the loud phrases, from its profile. Output -1.5 dB.` | what the user is told |

   With the flag OFF you should see **none** of the `EJCompProfile:` lines. That is the control case: if they
   appear with the flag off, stop and say so.


### CORRECTION, later the same night: reds 3-5 were MY GATE, not the product

Everything above about reds 3-5 being "plausibly ours" and about the placed build carrying a V2 crash is
**WITHDRAWN**. The sequence of claims I made, and what actually turned out to be true:

| I said | Truth |
|---|---|
| it's load under `-j 2` | no - identical at `-j 1` |
| the guards trigger a forbidden AU scan | no - registry plist metadata only, never instantiates, cannot raise iLok |
| introduced by letters `(l)`-`(q)` | no - `ba61a36` is clean, V2 side GREEN |
| then: by `7c348a8` | no - also clean, and its Source is **identical** to the gate commit |
| so the placed build carries a crash | **no. It does not.** |

**What it actually was.** Item 1 says to build `cmake --build build-release --target EchoJayLink EchoJayProbe`.
I ran exactly that. It does not build the **V2** archive, so `libEchoJay V2_SharedCode.a` stayed as
`feat/comp-profiles` had left it. The V2-side harnesses compile the CURRENT headers
(`harness_build.py` adds `-I Source`) and link that archive. Kathy headers + feat-branch archive = different
struct layouts for the same types, so `LinkSlotInfo.uid` was read at the wrong offset and came out as a garbage
character pointer: `EXC_BAD_ACCESS at 0x800` in `juce::operator==`. Not a dangling string. A layout mismatch.

**Proven both directions at one commit**, which is the only reason I believe it:

```
9d47209, V2 archive stale (the gate run, 23:01 and 23:22)  -> segfault, rc=139, 8 link-side FAILs
9d47209, V2 archive rebuilt, nothing else changed (00:22)   -> v2 side GREEN, 0 segfaults, 3 link-side FAILs
```

Every base-commit run I did (`0e2cfa9`, `ba61a36`, `7c348a8`) rebuilt `EchoJay` as well, which is why they all
came back clean - I had controlled for the deciding variable without realising it, and then read the clean results
as evidence about the letters.

**The real finding underneath, and it IS pre-existing:** `level_match_guard` case (1), 3 assertions. The trims
move but land on the wrong values (`asked 1.10, reads 0.30`), identically at `0e2cfa9`, `ba61a36`, `7c348a8` and
`9d47209`. That is a genuine pre-existing defect, and it is the thing the crash was hiding.

**The harness gap that let this happen, and it is one line's worth of thinking.** `dbc0e91` already added exactly
this protection - "the two-process guards refuse a Link archive older than the headers" - and it guards the
**Link** archive only. The V2 archive has no such check. So the guard refuses a stale Link archive and silently
links a stale V2 one, which is the asymmetry that cost tonight's gate. **This is the second time this class of
mistake has produced a false attribution on these same guards** (the first was four guards judged in a worktree
with no `build-release` artefacts, withdrawn a session ago). Twice is a pattern, and it belongs in the harness as
a refusal, not in my habits.

**Gate verdict after the correction - unchanged, but now for the right reasons:**

| # | Red | Whose |
|---|---|---|
| 1 | `calib_link_guard` (f) settle budget | **OURS** - one rule in two places, (m) updated one. Real. |
| 2 | `level_loop_guard` scribble (6a) | **OURS** - reproduces, unexplained |
| 3-5 | the three V2-side guards | **NOT the product** - my stale V2 archive. Underneath: case (1), pre-existing |

Still NOT pushed, because red 1 is ours. That part of the ruling was right all along.

**Confirmed on the other two as well**, at `feat/comp-profiles` with both archives freshly built from the same
tree (00:28):

```
lease_id_guard       rc=0  segfaults=0  FAILs=0   link side GREEN, v2 side GREEN  ==== GREEN ====
role_snapshot_guard  rc=0  segfaults=0  FAILs=0   link side GREEN, v2 side GREEN  ==== GREEN ====
level_match_guard                                 v2 side GREEN, 3 link-side FAILs (case (1), pre-existing)
```

So of the five reds, **two were pure artefact and are green**, and the third reduces to the pre-existing case (1).
The gate's real content is red 1 (ours) and red 2 (ours, unexplained).

**The rule this produces, for the gate script and for whoever runs it next:** the fast gate must rebuild
**every** archive its two-process guards link - `EchoJay` and `EchoJayLink`, not just the Link - or refuse to run.
Building one and not the other is not a partial check; it is a check whose failures are meaningless, and it cost
this gate five reds, three wrong attributions and most of a night.

---

## EARLIER — the day session of 1 Oct (unchanged below this line)

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
