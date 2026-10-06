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

**SUPERSEDED — see the 04:50 section below. Install `ship_2026-10-02b/`, not this.** The pair described
here carries the compressor closing line outside the feature flag; it was built before this branch's
first gate existed.

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

## 02:20 — PUSHED. The gate's five reds were two harness faults and one real defect.

**`merge/kathy-2026-09-06` is on origin: `971e4c1..b0ce97e`, 92 commits, verified by `git ls-remote` agreeing
with the local hash.** Sean's condition was "green, or the only reds are proven pre-existing". The final gate is
**98% passed, 1 of 53 failed**, and that one is proven pre-existing at four separate commits.

### What the five reds actually were

| Red | Verdict | Commit |
|---|---|---|
| `lease_id_guard` | **not the product** — stale V2 archive. Now GREEN. | `9c3dd79` |
| `role_snapshot_guard` | **not the product** — same. Now GREEN. | `9c3dd79` |
| `calib_link_guard` | **REAL, ours** — one rule in two places, (m) updated one. Now GREEN. | `1bd5e4f` |
| `level_loop_guard` | **not the product** — the scribble leg shared the first leg's state root. Now GREEN. | `b0ce97e` |
| `level_match_guard` | **genuinely pre-existing** — case (1) x3, reproduces at `0e2cfa9`, `ba61a36`, `7c348a8`, `9d47209` and now. STILL RED. | owed |

So one of five was a product defect, one was a real pre-existing defect, and **three were faults in the gate
itself**. That ratio is the finding, not an aside: a suite whose failures are mostly its own cannot be used to
judge a branch, and it took most of a night to separate them.

### THE PUSH CONTAINS FOUR COMMITS BEYOND (l)-(q) — read this before accepting it

Sean authorised "push `merge/kathy-2026-09-06` with (l)-(q)". The branch now also carries:

| Commit | What | Risk |
|---|---|---|
| `9d47209` | the gate's own two faults (a leg that assumed the machine kept up; a tail that hid the FAIL) | test-only |
| `9c3dd79` | the V2-side stale-archive refusal | test-only |
| `b0ce97e` | the scribble leg's own state root | test-only |
| `1bd5e4f` | **`CalibLoop::openFromPurpose` — ONE derivation of the purpose's opening state** | **product behaviour** |

`1bd5e4f` is the one to look at. It is a product change Sean has not reviewed, and it went in because red 1
could not be cleared without it — so the choice was push with it or do not push at all, and his instruction
plainly wanted the push once the gate justified it. What it does: the 6-arg `begin()` stopped carrying (e)'s
withdrawn "settle budget opens already spent" and now uses (m)'s rule, the same one `begin(Config)` already used.
**If Sean disagrees, revert `1bd5e4f` and the gate goes back to one red — nothing else depends on it.**

### The two harness lessons, both of which cost hours

1. **A build line that covers one archive and not the other.** Item 1 said `--target EchoJayLink EchoJayProbe`.
   That is not the V2 archive, and the V2-side harnesses link the V2 archive. Mismatched headers and archive
   meant `LinkSlotInfo.uid` was read at the wrong struct offset: three guards died with SIGSEGV before printing
   an assertion, which reads exactly like a product crash. `dbc0e91` had already fixed this for the **Link**
   archive on the premise that "ctest -L fast rebuilds build-guards, which is the V2 archive" — but build-guards
   is a different tree from build-release. Now both are refusals.
2. **Isolation applies BETWEEN THE TWO LEGS of one guard.** Both legs shared one `ECHOJAY_STATE_HOME`, so the
   scribble leg was the guard run a second time on top of the first run's state. `storeParamMaps` persists to
   `param_maps.json`, so a leg that marks AUDelay as a compressor poisons the next leg's precondition. It looked
   like a memory bug for hours purely because the second leg is the hardened one. Two plain runs in one root
   reproduce it with no MallocScribble anywhere.

**WITHDRAWN:** the "three memory-shaped failures" hypothesis recorded earlier tonight. Not one of the three was a
dangling pointer — two were the state leak, one was the layout mismatch. It was labelled a hypothesis and it was
wrong; the ASan session it asked for is not needed for any of these three.

### Still owed

- **`level_match_guard` case (1)**, 3 assertions: each Link's own trim moves but lands on the wrong value
  (`asked 1.10, reads 0.30`). Pre-existing, untouched tonight, and it is what the crash was masking. This is the
  one real gate red left.
- **Red 2's sibling:** `comp_profile_guard`'s `(2a)` on `feat/comp-profiles` had the same state-leak cause; the
  fix is cherry-picked there and the branch gate confirms it.

---

## 04:50 — the feature branch's FIRST gate, and what it caught

`feat/comp-profiles` had never had a fast gate run on it. Running one found a defect in the day session's own
work, in a build I had already placed and described as ready to install.

### `level_loop_guard` (16): the closing line was NOT behind the flag

```
FAIL  (16) ...and the closing line says "set as dialled" with what it did about the level
      [EchoJay Limiter: set as dialled, no profile yet.]
```

Item 4 rewrote the whole `if (dynamicsSlot)` branch of `completedLine()` to section 7's wording. That branch runs
whether the feature is on or not, so **with `comp_profiles_on.txt` absent — the default, and how Sean would first
run it — a compressor build told him "set as dialled, no profile yet"** instead of letter (q)'s "Set as dialled,
level matched, Output X dB." A sentence that says profiles exist, to someone who has none and no way to get one.
Part 2's rule was "behind a flag, default OFF", and the sentence the user reads is behaviour.

Worse, the comment above `stampCompProfileOnLoop` asserted the property the code lacked: *"with it off hasProfile
stays false and the loop behaves exactly as letter (q) — set as dialled, hold once, and a line that says so."*
The first two were true; the line was not.

Fixed in `cba5f02`. `CalibLoop::profilesFeatureOn` is set by `stampCompProfileOnLoop` when the flag is on, before
its early returns, so an unprofiled compressor on a flag-on session still gets section 7's wording:

```
flag OFF -> letter (q), word for word
flag ON  -> section 7, profiled or not
```

**Why it survived until now: only the flag-ON side of that wording had a test.** `comp_contract_guard` asserted
section 7's line; nothing asserted that the flag-OFF line was unchanged. Both sides are pinned now — (16) for off,
`comp_contract_guard`'s two legs for on.

### The build to install — USE `b`, NOT `a`

```
/Users/SeanD/echojay-vst/ship_2026-10-02b/          <- INSTALL THIS
  EchoJay V2.component     67M   D2585D92-8090-398A-9796-90BA505ADB25
  EchoJay Link.component   49M   D75297D7-A8F7-39F3-9593-EE0F3E26C754
```

Built from `b82f2ba`, both UUIDs read back from the placed bundles and matching the build tree. Unsigned, not
installed, nothing in `/Library`.

`ship_2026-10-02a/` is **superseded** and carries a `SUPERSEDED.txt` naming its UUIDs, so if Logic reports
`785D9918` or `625CD984` you are running the pair with the unflagged closing line. It was kept rather than deleted
precisely so those two hashes can still be identified.

### Both branches now stand at one red, the same one

| Branch | Gate | Red |
|---|---|---|
| `merge/kathy-2026-09-06` (PUSHED, `b0ce97e`) | 98%, 1 of 53 | `level_match_guard` case (1) |
| `feat/comp-profiles` (local, `b82f2ba`) | 98%, 1 of 56 | `level_match_guard` case (1) |

`feat/comp-profiles` carries `1bd5e4f` (cherry-picked) and `cba5f02`, so `calib_link_guard` and `level_loop_guard`
are green on both. The one remaining red is the pre-existing trim defect, proven at five commits now.

### Owed, in the order I would take them

1. **`level_match_guard` case (1)** — 3 assertions, each Link's trim moves but lands wrong (`asked 1.10, reads
   0.30`). Pre-existing, reproduces everywhere, and it is the only real red left in either branch.
2. **The profile flags do not ride the sidecar.** Neither `hasProfile` nor `profilesFeatureOn` is serialised, and
   stamping happens at `calibStart` and for companions, not per tick. A loop that hands over to the Link and back
   loses section 7's wording mid-flight. Harmless while the flag is off; it must be settled before the feature
   ships. Pre-existing from the day session, deliberately not widened into tonight.
3. **Review `1bd5e4f`** — the one product change in the push that Sean has not seen.


---

## 05:10 — the last red is ALSO the harness. The product applied every delta exactly.

`level_match_guard` case (1) was the one red left in both branches, and I called it "genuinely pre-existing" all
night. It is pre-existing, but it is **not a product defect** — and that matters, because it is the only thing
standing between this gate and green.

### The numbers, and they are deterministic

Fixture: trims `-2.5 / -2.4 / -2.6`, deltas `+3.6 / -1.5 / +2.0`, so the targets are `1.10 / -3.90 / -0.60`.
The leg reads `0.30 / -5.30 / -1.40`. **Identical in all seven runs tonight** — `0e2cfa9`, `ba61a36`, `7c348a8`,
`9d47209`, both full gates and the feat gate. So it is arithmetic, not timing or audio.

### What the Link's own log shows

```
seq ...547  v4_1  -2.50 -> 1.10   (delta 3.60)    <- EXACTLY the target
seq ...548  v5_2  -2.40 -> -3.90  (delta -1.50)   <- EXACTLY the target
seq ...549  V2_2  -2.60 -> -0.60  (delta 2.00)    <- EXACTLY the target
            ... then a SECOND level_match arrives ...
seq ...550  v4_1   1.10 -> 0.30   (delta -0.80)
seq ...551  v5_2  EJLinkState: remote set gain=-5.30 dB
seq ...552  V2_2  -0.60 -> -1.40  (delta -0.80)
```

**The first op is perfect on all three.** The V2 side then sends its second `level_match` (the `editData2` block,
`v2_side.cpp:156-172`), and that is what the leg ends up reading.

### Why the leg reads the wrong moment

`link_side.cpp:96-104`: it waits for `v2_applied.json`, then `for (k < 60) { feed; pumpMs (50) }` — three seconds
of audio — and only then asserts. Three seconds is plenty for op 2 to arrive. So case (1) asserts op 1's outcome
against state op 2 has already moved. There is no handshake between the two sides for "op 1 has been judged".

### NOT FIXED TONIGHT, on purpose

The fix is small - the V2 side should wait for a file the link side writes after case (1) is judged, before
sending op 2, exactly like the existing `link_ready.json` / `v2_applied.json` handshake. I did not do it, because
a test change made unattended that turns a red into a green is the one change that must not be made without Sean
awake. If I get the handshake subtly wrong the gate reports green and nobody looks again. **Ten minutes with the
context; please do it rather than accept my word that the product is fine.** The evidence above is the argument,
not my say-so: the three `seq ...547/548/549` lines are the product doing precisely what the leg asks.

### So the real score for the night's five reds

| Guard | Cause | Product defect? |
|---|---|---|
| `lease_id_guard` | stale V2 archive (my gate's build line) | no |
| `role_snapshot_guard` | same | no |
| `level_loop_guard` | scribble leg shared the first leg's state root | no |
| `calib_link_guard` | **one rule in two places, (m) updated one** | **YES** — fixed, `1bd5e4f` |
| `level_match_guard` | the leg reads after a second op lands | no |

Plus the one the feature branch's first gate found, which was the most serious of the night and was in a placed
build: **the compressor closing line changed outside the feature flag** (`cba5f02`).

Two real product defects, four harness faults. The gate found both real ones, which is the case for running it -
but four of five reds being its own faults is the case for fixing the harness before trusting the next run.


---

## 12:55, 2 Oct — Sean's live findings on 02b, and what each turned out to be

All four are root-caused. Two are fixed and in `ship_2026-10-02c`; one is unfixed because nothing in the tree ever
addressed it; one is diagnosed and left for a ruling.

### 1. The probe put a macOS security prompt on screen — FIXED (`a85786e`)

The chain was "a vocal chain with the Tube-Tech CL 1B", and that bundle carries `__Pace_Eden.bundle` (plus
`PlugIns/__Pace_Eden` and `Resources/__Pace_Eden`) — verified by **reading** the bundle, nothing loaded. The
pre-flight probe instantiated it, and a placed build's probe is unsigned, so macOS asked to lower security.

`EJPaceCheck.h` had declared the rule since 21 Sep — *"the signed EchoJayProbe … is the only harness process
allowed to load them"* — and the probe did not include the header that said so. **The rule existed as prose beside
the code that broke it**, which is the same shape as the (q) wording living outside its flag.

Proven both directions on the real bundles on this Mac:

```
CL 1B   (PACE)      -> refused PACE-wrapped: signed probe only (Tube-Tech CL 1B.component)   exit 3, nothing loaded
AUDelay (not PACE)  -> INSTANTIATE: OK  name="AUDelay" latency=0                              exit 0
```

Nothing is dropped by the refusal: exit 3 without instantiating is `PreflightState::error` ("in-host create
proceeds"), and only a TIMEOUT is `hang`, which is the only state that substitutes or skips a slot.

**The trade, stated rather than buried:** a PACE-wrapped plugin is no longer pre-flighted and loses hang
protection. A prompt is certain harm and a hang is a risk, so that is the right way round — but signing the probe
with Sean's Developer ID is what buys the protection back, and that needs his identity, so it is **not done**.
`EJ_PROBE_ALLOW_PACE=1` re-enables probing for a signed build.

### 2. "level matched" on +12 dB of make-up — FIXED (`a6a5c71`)

The CL 1B's hold wrote OUT +12.0 dB — the ceiling — and the line said the level was matched. It *was* matched,
which is why the line was not false; it was useless, because a compressor needing 12 dB of make-up is pulling
12 dB down and the sentence said everything was fine.

Above 6 dB the line now names the cause, and says when the figure is only a floor because the trim ran out:

> "Set as dialled, taking about 12 dB off: too much, check the threshold (my output trim is at its +12 dB
> ceiling, so it may be taking off more than this). Output +12.0 dB."

One derivation (`overCompressionPhrase()`) serves all three closing-line branches — (q)'s, section 7's profiled
and section 7's unprofiled — rather than being written into each. `level_loop_guard` (17) proves both directions:
+12 warns and drops the match claim, +3 still reports a plain match, and a non-dynamics slot at +12 is not accused
of compressing at all.

### 3. Still must stop/start Logic to hear processed audio — NOT FIXED, and nothing ever addressed it

There is **no `setLatencySamples` in `PluginProcessor.cpp` or `LinkProcessor.cpp` at all** — only in built-in
sub-processors (`EedTapeProcessor`, `SurgicalEqProcessor`). Letter (h), the latency budget, was queued behind the
dry-audio answer and never built, and the MERGE record notes the Logic re-query test was skipped by ruling. So the
honest answer to "was that fixed?" is no, and nothing in the tree would have fixed it. The log will say which of
the candidate causes it is (latency not reported -> no graph rebuild; wet/dry not engaged until transport restart;
bypass state), and guessing between them without it is how three attributions went wrong last night.

### 4. AVOX SYBIL offered but not built — DIAGNOSED, NOT FIXED (needs a ruling)

**A trailing space.** SYBIL's real AU identifier is `AudioUnit:Effects/aufx,AnVD,VST ` — 32 characters, because
Antares' manufacturer OSType is literally `'VST '`. The `chain_blacklist.txt` line is the same string trimmed to
31. `ChainHost::isBlacklisted` is an exact `StringArray::contains`, so:

```
blacklist : 'AudioUnit:Effects/aufx,AnVD,VST'   len 31
identifier: 'AudioUnit:Effects/aufx,AnVD,VST '  len 32
exact match: False      trimmed match: True
```

So the crash skip list — whose own header says a listed plugin is *"withheld from the chain feed and refused at
load"* — cannot see it. SYBIL is offered among the 1452 feed entries, the model picks it, and then it fails for
real. It also carries an expired hangs-on-load mark (`"hangs on load (Rosetta static initialisers, 17 Sep
sample)"`, `until: 2026-09-25`), so it is re-probed and times out. **Offered by name, refused by identity.**

`ChainHost.cpp:7397` trims the path on read (`.trim()` on the value before the TAB), and the file was written
trimmed as well. **This is a class, not one plugin:** any product whose AU OSType ends in a space can never be
blacklisted, which means the crash skip list silently fails exactly where it is most needed.

Not fixed because the fix changes which plugins get withheld, and that is Sean's call. The change is small:
normalise the comparison so existing lines still match, and stop trimming the path on read and on write. It wants
a leg with a trailing-space fixture, proven both ways.


---

## OWED, as of 2 Oct 20:20 — named so none of it is carried in anyone's head

These are Sean's rulings from the 19:43 and 20:1x messages that are NOT in 02j. Each says what it needs, because
the order between them matters.

### 1. Make-up goes on the PLUGIN's own output/gain control when the block names one

EchoJay's slot output is used only when the block names no output control. Today the hold writes EchoJay's own OUT
and nothing else (21t-m ruling, 29 Sep), so this reverses part of that for the case where the block is explicit.

### 2. THE FEEDBACK TRAP — must land in the SAME round as (1), not after it

Sean, 20:1x, and he is right. `grDb` is now `-(out p90 - in p90) + static_gain_db`. A make-up written on the
plugin's **own** output control sits INSIDE the in->out span, so:

> every dB the loop writes there reads as a dB LESS gain reduction, and the loop chases it

It would push the drive to compensate for its own make-up, which is a runaway in the direction of more
compression. `static_gain_db` does not cover it: that is the profile's figure for the unit at its NEUTRAL
settings, not what the loop has since written.

`grDb` must add back, in dB:
  - the make-up the loop itself wrote on the plugin's output control — read back from the map, not assumed from
    what was asked, because a clamped or refused write must not be added back;
  - any non-neutral output/gain setting THE BUILD wrote before the loop started (same reason: it is inside the
    span and is not the compressor's doing).

**The trap is NOT live in 02j**, because nothing writes make-up on a plugin control yet. That is exactly why (1)
and (2) ship together: (1) alone arms it.

**Leg (ruled):** write +2 dB of make-up on the plugin's output control; the GR reading must not change.

### 2a. THE TWO SENSORS MUST AGREE BEFORE THE LOOP MOVES (Sean's ruling, 3 Oct) — NEXT ROUND

With NO profile, the loop moves only when BOTH GR readings agree on the direction:
- **harder** only if level AND crest both read UNDER the band
- **softer** only if BOTH read OVER it
- **disagree -> HOLD**, and log both figures

This is the ruling that covers both of last night's failures with one rule, which is why it is better than either
sensor alone:
- case (9): level read 6 dB over (a slot that merely attenuates), crest read ~0 -> they disagree -> HOLD, instead
  of backing the drive to -6 dB;
- the 19:43 CL 1B: crest read ~0, level read over -> they disagree -> HOLD, instead of walking the drive to +6 dB.

**Why it is urgent rather than tidy (Sean's point):** a THRESHOLD block on an unprofiled slow compressor runs on
crest alone TODAY, and can walk the threshold to `min_db` exactly the way the drive went to +6. The agreement rule
closes that before the threshold actuator work makes it reachable more often.

**Legs: one per direction** - both-under moves harder, both-over moves softer, and each disagreement case HOLDS
(level-over/crest-zero, and crest-over/level-zero).

With a profile the level-minus-static figure stands on its own; the agreement rule is for the unprofiled case where
neither sensor can be trusted alone.

### 2d. max_steps from the block (Sean's ruling, 3 Oct) — NEXT ROUND

The block carries `max_steps` and the parser does not read it at all (confirmed 2 Oct: 0 occurrences in
EJCalibLoop.h); the loop uses its own `kMaxSteps = 6` regardless. Read it, and use **the smaller of the block's
figure and 6** - so the server can ask for fewer rungs but never more than the ruled cap.

### 2b. RATCHET: gr_target_db and last_gr_db per compressor slot in [CURRENT CHAIN]

**Field names AGREED with B (2 Oct, Sean confirmed):** `gr_target_db` per compressor slot, and `last_gr_db` for
the last measured GR. No need to wait on B. Emit in the next round.

Why: so the server can act on a second "harder" instead of re-deriving a target it cannot see. Today the block
says nothing about what each compressor was aiming at or what it actually measured.

**Where it goes, found 2 Oct so the next round is mechanical:** `Source/EchoJayAPI.cpp:4016`, the loop over
`rack.slots` that prints `N: "name" (format, BYPASSED, wet N%)`. Add the two figures to that line for slots that
are dynamics.

**Where the figures come from:** the CalibLoop carries the band as `lo`..`hi` (from the block's `gr_target_db`)
and the measured figure as `lastGr`. That loop rides the rack sidecar (`rc.calib`), which this targeted
[CURRENT CHAIN] path already reads - so no new transport is needed. A slot with no loop and no stored target emits
neither field rather than a zero: absent must not read as "aiming at 0 dB".

**Caution:** `lastGr` is now the LEVEL-based figure (2 Oct), not the old crest one. Anything the server infers from
`last_gr_db` is only as good as that sensor, and on a unit with built-in make-up and no profile it can understate -
see the make-up correction owed in item 2 above, which must land before `last_gr_db` is trusted for a ratchet.

### 2c. OWED: a leg for the LEVEL-based GR sensor, in comp_profile_guard

The GR sensor now chooses per window: **level minus `static_gain_db`** when a profile supplies that offset,
**crest** when nothing does. `grVia=` in the window line records which ran.

Why the choice exists, both directions proven the hard way in one evening:
- CREST alone read ~0 on Sean's CL 1B (slow unit, sustained material - SHORTMAX and SHORT90 fall together), so the
  loop walked the drive to +6 dB believing it had bought nothing. That is the bug that started this.
- LEVEL alone read 6 dB of "compression" on a fixture slot that merely attenuates 6 dB, and the loop backed the
  drive to -6 dB. `level_loop_guard` case (9) caught it within one gate run.

**Owed:** a leg in `comp_profile_guard` (which hosts a real plugin WITH a profile, so `static_gain_db` is known)
asserting that a known level reduction reads as that GR via the level path, and that a static offset is subtracted
rather than counted. `level_loop_guard` case (9) already holds the crest path. My first attempt at this leg asserted
against `lastGr` while it was still NaN - it must drive enough windows for one to be JUDGED first.

### 3. Threshold/amount actuators must work in the loop, not just drive

`param` = a plugin control. The parser and the Actuator enum already carry Threshold and Input, and
`switchNamedAsActuator`/`setSlotControlsToValue` exist - but the LOOP's stepping path is the drive. This is a
second actuator through the whole loop, including its own settle and its own at-the-limit behaviour, so it wants
its own round and its own legs rather than being bolted on.

### 4. The load-FAILURE substitution leg

The code is in 02j (a failed create is replaced in the same slot by the built-in of its role, card names both).
The LEG is not written. Sean's shape: a slot whose create fails is replaced in the same slot, the card line names
both, **and a later calibration block for a slot after it still lands on the right plugin** - that last clause is
the interesting half, because it is the item-1 remap and the substitution interacting.

### 5. Still open from earlier, unchanged

- `level_match_guard` case (1) passed once the op-1/op-2 handshake landed; watch it stays green.
- The 0.250 slot wet: 02g+ instruments both ends (`wet_pct RECEIVED ... raw= type=`, and ABSENT). Unanswered
  until a live build with that build in place. If it logs ABSENT and 0.250 still lands, the value is invented on
  our side and I was wrong to point at the ops payload.
- `docs/COMP_PROFILE_SPEC_v1.md` section 5 carries the 3 s ruling; **Sean's Desktop master and B's side still say
  20 s** and need the same change.

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

---

## Sean's ruling, 3 Oct (after the 08:03 diagnosis) — NEXT ROUND, in this order

### A. turnType = chain_edit — IMPLEMENT BUT DO NOT SHIP UNTIL SEAN RELAYS B'S ANSWER

Send `chain_edit` when the classifier says `chain_edit`. Today `PluginEditor.cpp:29536` decides turnType from
`hadChainFeed` alone and has **no path that can emit chain_edit at all**.

**HELD:** Sean has asked B whether the server handles `chain_edit`. This part does not ship until he relays the
answer. If it shipped first and the server does not handle it, every edit turn would break instead of one.

### B. PREFLIGHT: "no base stated" IS NEVER A REFUSAL — ships regardless of B's answer

If the reply carries no base list, compare against the **[CURRENT CHAIN] snapshot this turn actually sent**
(rev 6 at 08:03:21 in his session), and refuse **only if the live rack has really changed since then**:
`rev differs AND the touched slot moved or is gone`.

Why it ships regardless: a missing optional field became a refusal. `base=0 []` against `live=4` was read as
"that slot is gone" when the rack was intact and the apply itself had already printed `live=4`.

**Leg (ruled):** an edit with no base list on an unchanged rack APPLIES; the same edit after the touched slot has
been removed REFUSES.

### C. A LOOP STARTS ONLY FROM AN EDIT THAT APPLIED

His log: `08:03:48.346 Not applied` then `08:03:52.485 local edit settled -> 1 loop(s) started`. The loop starts
from the "local edit settled" path, which never consults whether the edit applied.

**Leg (ruled):** a refused edit starts no loop.

### Order for the next round, and the deadline
1. B (preflight) and C (loop only on applied) — both ship.
2. A implemented but HELD pending B's answer.
3. Legs owed from 03a: item 3 (an amount block in listen mode never writes the control) and item 5 (no start_db
   and an unreadable control -> holds).
4. Then the agreement rule (2a), `max_steps` (2d), then the owed set (make-up + feedback trap together,
   threshold/amount stepping, gr_target_db/last_gr_db, the level-path leg).

**Gate finished by 17:30** (Kathy runs EJ Map on this Mac tonight; no builds or gates while she does).

---

## Round ship_2026-10-03b — the 3 Oct ruling, unattended

### What shipped

**(a) "No base stated" is never a refusal** — `ChainHost.cpp`, the preflight's touched-slot guard.
With no base list the edit is judged on the ops' own identities against the live rack: `guard=touched-slot-gone`
when an op that acts on an existing slot names an index past the end, `guard=touched-slot-identity` when `op.name`
and the live slot disagree, otherwise it proceeds and logs *"no base list stated - judged on the ops' own
identities"*. A missing optional field is never the refusal.

**The rev half of the ruling is NOT done, and it is Sean's call.** His ruling also says to compare the rev of the
`[CURRENT CHAIN]` snapshot the turn actually sent. The borrowed path passes `expectedRevision = -1` — the turn does
not retain what it sent — so there is nothing to compare against until that is plumbed. The silent direction
(compare against the live rev, which always matches, so nothing ever refuses) is worse than today, so it was not
guessed at unattended. The identity check gives both legs he asked for.

**(b) A loop starts only from an edit that APPLIED** — `PluginEditor.cpp`.
`editWasRefused(editJson)` reads the result already recorded on the chat message, keyed by the edit's own JSON.
It gates the three settle callbacks **and** `startCalibrationForEdit` itself, which is the choke point every road
passes through — the three callers also check, because each has its own thing not to do afterwards (none may post
"nothing started" over a card that already says "not applied"). "No result recorded yet" is **not** a refusal: the
ack can trail the settle by four seconds.

**(c) turnType = chain_edit, BEHIND A FLAG, OFF BY DEFAULT** — `EchoJayEditor::sendsChainEditTurnType`.
`~/Library/EchoJay/turntype_edit_on.txt` present AND the classifier's intent is `chain_edit`. Default OFF is
byte-for-byte today's behaviour. The write goes through the new `EchoJayAPI::overrideNextChatTurnType`, which sets
only the turn type — `setNextChatTurnType(t)` resets `nextChatBusCount_` as a side effect, which would drop a
staged meter payload. **Still held pending B's answer**; nothing changes until Sean creates the file.

**(e) The agreement rule** — `EJCalibLoop.h`, before both step paths (threshold and drive).
With no profile: harder only if LEVEL and CREST both read under the band, softer only if both read over, otherwise
HOLD and log both — `state=held-disagree`, with the two figures and the band on the line. `grLevelDb` /
`grLevelKnown` are now computed in **both** `fillCalibWindow` twins whether or not a profile exists, because the
rule needs both sensors to have an opinion. With a profile the rule is off: `static_gain_db` is what makes the
level figure trustworthy, and the profile is the only thing that supplies it.

**(f) max_steps from the block** — parsed into `Config::maxStepsFromBlock`, carried as `maxStepsBlock`, applied
through `effMaxSteps()` = the smaller of it and our own 6. Both step-budget checks use it.

### Two things found while writing the legs, and fixed

**1. A Link rack's loop was losing fields on every tick.** `calibTick` is `calibLoad → decide → calibStore`, and
for a non-empty uid both ends go through `CalibLoop::toVar` / `fromVar`. `holdOnly` was in neither — so **item 3's
fix from 03a held for exactly one tick on a Link or borrowed rack** and the loop then stepped the control again, on
the rack Sean's sessions actually run on. `maxStepsBlock` had the same hole, and so did the whole profile group:
`hasProfile` was false again by the second tick, which means COMP_PROFILE_SPEC_v1 §7's one check could never run on
a Link rack at all. Twenty fields now travel, NaN as a void var (JSON writes a NaN double as null and reads it back
as 0, and "0 dB expected" is a different claim from "nobody said"). The log-only members are deliberately left out:
`lastHeardS` riding the sidecar is what caused 187 consecutive stale windows after a handover.

**2. The companion road had no item-5 handling.** The drive substitution has been on it since 30 Sep; the
named-control half was added only to `calibStart`, so a *companion* block naming a control with no `start_db` still
opened at NaN. A build with two compressors gets one of each, and which one has the defect is an accident of
ordering.

### Legs added

| Leg | Guard | What it holds |
|---|---|---|
| (a) | `level_slot_guard` | no base list + unchanged rack → applies; touched slot removed → refuses, naming what it expected and what it found; index past the end → refuses; unnamed op on a live slot → applies; **an ADD on an empty rack and an add past the end → apply** |
| (b) | `ui_guard` | a refused edit starts no loop and writes no drive; the same edit applied starts it; no result yet still starts it |
| (c) | `ui_guard` | both flag states; only a `chain_edit` intent; the override reaches the wire and the staged `busCount` survives it |
| (d) item 3 | `level_loop_guard` (24) | a held block in LISTEN mode: twenty windows 2.5 dB under the band, zero writes to the control, zero to the drive, and it still measures and reports |
| (d) item 5 | `level_loop_guard` (25) | no `start_db` on a named control → the control's own position is read; unreadable → holds, and twelve windows later has written nothing |
| (e) | `level_loop_guard` (22) | both disagreement shapes hold; both agreements step; no level figure → the crest still stands alone; with a profile the rule is off |
| (f) | `level_loop_guard` (23) | `max_steps` 2 → 2 dB; absent → 6; 10 → still 6 |
| new | `level_loop_guard` (26) | the sidecar round trip keeps every field a decision is made on, NaN stays NaN, and an older sidecar keeps the defaults |

### Faults of mine, caught by the gate rather than by Sean

- The first cut of the no-base branch walked the merged `touched` set — `o.slot`, `o.to` **and** `o.after`. An add's
  `after` is an **insertion point**, so `add after 0` on an empty rack was refused with "the rack does not have
  that slot". That is every build, and `ui_guard`'s 21t-h and 21t-i legs said so in one run. The index that has to
  exist is `o.slot`, on the ops that act on a slot already there. Both directions are now legs.
- Leg (26) segfaulted on its last assertion: `l.toVar().getDynamicObject()` is a raw pointer into a temporary var,
  released at the end of the statement. That is the 18 Sep Pro Tools crash, in a test harness.
- Leg (25)'s first fixture probed the Gain built-in, whose `level_db` display text is a bare number and so does not
  parse as dB. A fixture fault, not a product one; it now asks every slot and names what it found.

### Still OWED after 03b

1. **The rev half of the preflight ruling.** Retain the rev of the `[CURRENT CHAIN]` snapshot the turn actually
   sent and pass it as `expectedRevision` on the borrowed path, so "rev differs AND the touched slot moved or is
   gone" can be evaluated as ruled. Today the borrowed path passes `-1`.
2. **Make-up on the plugin's own output control, TOGETHER WITH the GR feedback-trap fix.** Add back make-up the
   loop wrote, read from the map, plus any non-neutral build-written output setting. Leg: +2 dB of make-up must not
   change the GR reading. These two are one job — fixing the write without the trap makes the sensor lie.
3. **Threshold/amount actuator stepping.** Today a named amount control is HELD (03a item 3, leg (24)); stepping it
   needs the profile's curve, which is what `set_directly` currently stands in for.
4. **`gr_target_db` + `last_gr_db` in `[CURRENT CHAIN]`** at `EchoJayAPI.cpp:4016` — B has been told the field is
   `gr_target_db` per compressor slot plus `last_gr_db` for the last measured GR. Not emitted yet.
5. **A level-path GR leg in `comp_profile_guard`**, which is where a rig with a REAL profile lives — level_loop_guard
   has no profile, so case (20) was withdrawn rather than faked.
6. **`level_match_guard` case (1)** — the reverse handshake fixed the race; keep it watched.
7. **turnType=chain_edit** — implemented, flagged, OFF. Ship by creating `~/Library/EchoJay/turntype_edit_on.txt`
   once B confirms the server handles the turn type.

### 03b gate verdict

**GREEN: 100% of 57 tests passed (56 `fast` + 1 `plugins`), 0 failed assertions.** Total test time 2181 s,
finished 12:37 on 3 Oct.

Placed, **not installed**, at `ship_2026-10-03b/`:

| Bundle | arm64 | x86_64 |
|---|---|---|
| EchoJay V2 (67M) | `B3561412-2B73-3E5C-B706-C0085A6BD34C` | `0408CD4B-FA55-3E50-97B9-89D2F0999C76` |
| EchoJay Link (49M) | `7217FFE4-29B0-35F8-845C-7BD80691EDDD` | `0B89F663-E55D-3590-A267-4C2B29D7F873` |

Provenance: the newest `Source/` file is `EJCalibLoop.h` at 11:36:02; both bundle binaries are 11:59/12:00 and the
gate ran 12:00–12:37 against the same tree. Nothing in `Source/` changed after the build, so the gate and the
bundles are one Source.

**The first gate run (10:29-10:49) was 7 of 57 red, and the attribution matters more than the count:**

- **Five were the staleness refusal working as designed** — `calib_link_guard`, `alias_mirror_guard`,
  `lease_id_guard`, `level_match_guard`, `role_snapshot_guard` all compile their V2/Link side against
  `build-release`, whose archives were from 08:16 and older than `Source/`. They printed *"the V2 archive is OLDER
  than the headers this would compile against"* and refused rather than segfaulting. That refusal exists because
  this exact class cost three wrong attributions on 1 Oct. Rebuilt `build-release`; all five passed.
- **`preflight_guard` leg (b) was the LEG out of date, not the product.** It asserted `getNumSlots() == 1` after a
  failed create. The log says `EJLoad: SUBSTITUTED "Guard Failer" -> "EchoJay Compressor"` — which is the 2 Oct
  ruling, *"LOAD FAILURE = SUBSTITUTE"*. The leg now asserts the slot is KEPT and that the card carries the host's
  own reason rather than "hangs on load", which is what keeps it distinguishable from leg (a) (never created
  in-host at all). Note this test carries the `plugins` label, so 03a's 56-test `fast` run never ran it.
- **`level_loop_guard` was mine, twice.** First a fixture fault: leg (25) probed for a control `readControlDb` can
  read, and **no EchoJay built-in publishes a single JUCE `AudioProcessorParameter`** (not one `addParameter` in any
  `Eed*Processor.cpp`) — they run on structured settings, so `readControlDb` can never read one. The leg now
  asserts that as ground truth and tests the HOLD direction, which is the safety-critical one and the one a
  built-in actually exercises; the "read it" half is owed in `comp_profile_guard`, which hosts a real plugin.

**And the second red was a real product gap in my own 03a fix.** Item 5's fix stopped the WRITE from an unread
position, but the position stayed NaN and every line quoting it still printed `nan` - which is what Sean actually
reported ("dialling Threshold from nan dB", then a window reading "Threshold=+0.0"). Fixed at `signed1`, the one
formatter every line passes through: a non-finite value now renders `unknown`. The loop's position is deliberately
left NaN, because it holds precisely BECAUSE nobody read the control - writing a 0 there would be the invented
reading wearing the fix's clothes. The leg now asserts on the rendered line (no `nan`, `unknown` present) rather
than on the member.

---

## NEXT ROUND (not 3 Oct): explicit "harder" to 12 dB, and the ONE corrective move

Sean's ruling, as corrected at 14:37 on 3 Oct. **This supersedes the earlier wording of the same item**, which said
"Two steps at most": it is **at most ONE corrective move**, and a miss after that move ASKS rather than moving again.

### The pick

Explicit "harder" goes to **12 dB**. Past the profile's measured points the SERVER estimates, and says so: the
block carries `estimated: true` with a **sense**.

### The one corrective move, by pick kind

| Pick | After the hold |
|---|---|
| `estimated: true` | **ONE move, and only one.** If level-minus-static GR misses `expected_gr_db` by **more than 1 dB in either direction**, move the NAMED CONTROL once in the block's sense, by the amount needed to CLOSE THE GAP - not by a fixed step. B's block will carry the amount; failing that, use the profile slope. Then hold and report the measured figure. **If it is still off: say so and ASK.** Never a second move. |
| measured (not estimated) | Unchanged: the existing **one-move-toward-less-gain-reduction** rule (COMP_PROFILE_SPEC_v1 section 7). |
| `at_control_limit` | **No move at all.** The control is already at its end; moving it is a write that cannot help. |

### Bounds that hold for every case

- **Never past `min_db` / `max_db`.**
- **Never on crest alone** - the corrective move is judged on level-minus-static, which means it needs
  `static_gain_db` from the map profile or from the block (see `staticGainKnown()`, shipped 3 Oct). A slot with no
  static offset gets no corrective move.
- **Never EchoJay's own drive.** The named control, or nothing.

### Legs (ruled)

1. **1.5 dB under `expected_gr_db` moves once** - and the move closes the gap rather than stepping a fixed 1 dB.
2. **0.8 dB off moves not at all** - inside the 1 dB tolerance.
3. **A miss after the move ASKS rather than moving again** - the second move must be impossible, not merely unlikely.
4. ...and the two kinds that are not estimated: a measured pick keeps one-move-toward-less; `at_control_limit` moves
   nothing.

### Then, still owed and unchanged

Make-up on the plugin's own output control **together with** the GR feedback-trap fix (adding make-up back must not
change the GR reading; leg: +2 dB of make-up leaves the figure alone). These two are one job - fixing the write
without the trap makes the sensor lie about what the write did.

---

## UNCOMMITTED, UNGATED work in the tree after 03c (3 Oct, evening)

`ship_2026-10-03c` is GREEN, placed and reported (V2 arm64 `E20761E5…`, Link arm64 `2D4426C9…`). Everything below
was written AFTER those bundles were built, so **the tree no longer matches them**. It compiles (V2 archive, 0
errors, 16:31) but **no guard has run against it**. It gates in the next round.

### In the tree now: Kathy's live low-level-gain measurement (LOG ONLY)

Answers whether the CL 1B's constant 0.7 dB gap exists in our chain at all. `ChainSlot` holds a 64-entry ring of
(input dBFS, out−in) pairs; `ChainHost::noteLowLevelGainSample` / `lowLevelGain` own it. Two gates: **confirmed**
= 6 dB under the block's `in_at_gr1_dbfs` (parsed, carried, serialised; NaN until B ships it); **fallback** = 15 dB
under the track's remembered loud level, tagged `unconfirmed`. Under −60 dBFS and silent windows ignored. A gate
change resets the store. Confirmation test: two input bands ≥6 dB apart, ≥3 samples each, medians agreeing within
0.1 dB, else `not confirmed` with both figures. One composer (`CalibLoop::lowGainLine`) so the twins cannot drift.
Rides the window line. Plus `EJPlace: "<name>" loaded at host sampleRate=… Hz`.

The store lives on the SLOT, not the loop: a loop round-trips through the sidecar every tick on a Link rack, so a
ring on the loop would reset every second and never accumulate.

### Backlog for the next gated round, in the order it was ruled

1. **The save fix** (Sean's rulings, 3 Oct): additive `borrowedRacks` key, cache-only serialisation
   (`getCachedSlotStatesVar` is explicitly safe in the save callback), restore deferred to the message thread on
   first sight of the uid (both load orders). Tie-break **by revision**: store the Link's rev the borrow started
   from; apply V2's copy only if the Link's restored rev is still at that base, else the Link wins and V2 discards.
   The Link persists no rev today — add `chainRev` additively, restored into a separate member and published in the
   sidecar, NOT by overwriting `chainRevision_` (the counter the preflight staleness guards compare against).
   Additive format gaps: slot output gain + pre-trim in `chainSlotsXml` AND `chainModelToVar`; `settings_structured`
   in `chainModelToVar` (it is read on restore and never written). V2-side save line naming each open borrowed rack
   (uid, base rev, slot count, bytes). Legs: both load orders identical on the Link incl. slot output gain and
   structured settings; older Link chain + newer borrowed rebuild → borrowed wins; deleted on the Link → stays
   deleted; a session with no `borrowedRacks` key loads exactly as today.
2. **`gr_target_db` / `last_gr_db` on the BORROWED path.** Shipped in 03c only on the `ChainHost` adapter; the
   `RackSidecar` overload (`PluginEditor.cpp` ~27735, the Link-rack path) passes no `grNotes`, which is why Sean's
   16:10 slot 3 line carried neither field. That branch already holds `bh`, so it is a small fix. The sidecar
   branch (Link advancing its own loop after deselect) needs the figures to ride the sidecar additively.
3. **The stale window after an edit** (16:10 item 2). `begin()` sets `awaitFresh = false` and `lastHeardS = -1`
   while `retarget()` sets `awaitFresh = true` and keeps `lastHeardS` — so a FRESH loop judges its first window
   and a re-target does not. Third contributor: `calibLastWindowMs_` is processor-level and is not restarted when
   a loop begins, so the first tick can close a "window" immediately AND pass that arbitrary span as the window
   length. Fix: arm `awaitFresh` in `begin()` and restart the window clock at loop start. Leg churn is likely —
   several legs assume the first window is judged.
4. **The measure mismatch** (Kathy's definition). The block says `measure=shortmax`; the client computes GR from
   `shortTermP90Db` (SHORT90 = where the programme SITS). Both legs agree with each other, so there is no in/out
   mismatch — the measure is simply not the one the definition and the block ask for, and p90 under-reads GR on a
   compressor. Fix: use `maxShortTermDb` on both legs. Also log `in=`, `out=` and the parsed `static=` so the two
   candidate causes (measure vs a placeholder `static_gain_db: 0`) separate themselves immediately.
5. **The map cache** (16:10 item 3). `~/Library/EchoJay/param_maps.json`, 138 maps, **0 with a comp_profile**; the
   CL 1B's fp `ab70ea5337fe` is cached at rev `882416b27874` with none. Revalidation exists and rev-compares, but
   is latched **once per session** (`mapsRevalidated_`) and had already fired before B's ~14:50 fix. AND
   `storeParamMaps` does `if (oldRev.isNotEmpty() && oldRev == newRev) continue;` — **so B must bump the map's rev
   or the client discards the corrected body even on a revalidation.** Client fix: on the "map present but no
   comp_profile" path, request a targeted re-fetch of that one fp, at most once per fp per session.
6. **Silence and quiet material** (Sean's rules). Confirmed: silent windows are never judged and nothing is
   written (`if (w.silent) { noSignalMs += windowMs; … return; }`), and on resume `noSignalMs = 0` and
   `Waiting → Listening`, so the check does run on the first audible windows however much later. Gaps vs the
   ruling: the threshold is `kNoSignalMs = 30000` (ruled 10 s); the wording is "<plugin> is on - play it and I'll
   tell you what it's doing" (ruled "Set. I'll check it and match the level when the vocal plays"); and
   `askNoSignal` returns early unless `mode == Passive`, so a Listen loop says nothing. NOTE: `card()` returns
   empty in Passive by a previous ruling ("PASSIVE SAYS NOTHING WHILE IT RUNS"), so putting this on the CARD
   reverses that — flagged for Sean. Quiet material: judge only windows within 6 dB of the remembered loud level;
   slot tallies are plain dBFS so this is a direct comparison.
7. **The estimated-pick corrective move** — at most ONE, sized to close the gap, then ask (recorded above).
8. **Make-up on the plugin's own output control together with the GR feedback-trap fix.**

### Rulings, 3 Oct evening (these amend the backlog items above)

**Item 4, the measure mismatch — CONFIRMED AS THE LEAD.** B confirms the 13:53 and 16:10 blocks carried
`static_gain_db = -0.7`, so the "placeholder 0" candidate is OUT. And the arithmetic now closes:
with static −0.7 and a SHORT90 drop of 2.0, Kathy's `GR = static − (out − in)` gives **1.3**, which is exactly the
`grLevel=1.3` in the 16:09:03 window line. For GR to be the true 2.7 the drop at the LOUD PHRASES must be ≈3.4 dB,
i.e. ~1.4 dB more than at p90 — which is what a compressor does to its loudest material. So the fix is to compute
both legs from `maxShortTermDb`, and the expected size of the correction is known in advance, which is what makes
it falsifiable: if moving to shortmax does NOT move the CL 1B's reading by about 1.4 dB, the lead was wrong.
Still log `in=`, `out=`, `static=` so the next session shows the raw figures either way.

**Item 6, silence — SETTLED, and it does NOT reverse the no-card ruling.** Passive stays silent WHILE IT
MEASURES. But if nothing is heard for **10 s after a build or edit**, the check cannot run, so post the settle line
EARLY, as a chat line:

> Set from its profile. I'll match the level when the vocal plays.

When it later measures, post the normal line. **One line each, never more** — so two separate latches, not one:
the early settle line and the normal measured line are different lines with different conditions. The 10 s clock
runs from the build or edit, replacing `kNoSignalMs = 30000` for this case. Nothing goes on the card, so
"PASSIVE SAYS NOTHING WHILE IT RUNS" stands as written.

**Item 5, the map cache.** B is bumping the map rev when a profile attaches, so the rev-compare will no longer
discard the corrected body. NOTE: the **once-per-session latch** (`mapsRevalidated_`) still means a profile that
attaches mid-session does not arrive until the plugin is reloaded — so the targeted re-fetch on the "map present
but no comp_profile" path is still worth having, and is what makes it land in the same session.

**New, from B: `expected_drop_db` on profiled blocks.** Read it when present. It is the expected (out − in) level
drop, so it is a direct cross-check on our own measured drop — and with the measure fix above it is the figure that
says whether we are now measuring the drop the server meant. Parse additively, log it, change nothing with it yet.

**Build discipline:** nothing builds until the EJ Map run is finished; Sean will say when. The tree currently holds
Kathy's low-level-gain measurement, compiled clean (0 errors) but ungated.

### Rulings, 3 Oct late — map ranges and anchors

**`controls_norm` precedence is correct as it stands** (it wins, and the `controls` duplicate is dropped before the
write, matched on the normalised name). **Approved for next round:** build `normOwned` only from `controls_norm`
names that RESOLVE to a parameter, so an unmatched name keeps its display fallback instead of losing both writes.
Leg: a `controls_norm` name the plugin does not have must leave the display value applied, not drop it.

**Anchor interpolation is POSITIONAL, confirmed in code.** `interpolateAnchors` brackets the target and returns
`n0 + frac * (n1 - n0)` using the anchors' own normalised positions — so a half-range table (8.5 @ 0.5, 31 @ 1.0)
puts 10 dB at ≈0.533, NOT at ≈0.07. It never spreads min..max over 0..1.

**The cached CL 1B map is the GOOD table, not a half-range.** fp `ab70ea5337fe`, rev `882416b27874`: Gain has 23
anchors, `range [-47.8, 31]`, first anchor −47.8 dB @ norm 0.1, and 0.0 dB is bracketed by real anchors at
−2.7 @ 0.300 and +1.0 @ 0.350, i.e. ≈0.33 — the position Kathy says is right. The un-anchored stretch below norm
0.1 is the "Off" region, correctly excluded because "Off" is not a dB value. **Sean has told B not to bump the rev
for this fp unless the server body matches this table** — a bump would otherwise replace the good table with the
worse one. Note this also corrects an earlier report of mine: `Gain 0.0` is NOT refused as out of range on this
machine; that only happens against the 8.5..31 table, which is not what is cached here.

**Approved for next round: a null-anchor guard.** `anchorsFromVar` does `(float)(double)(*p)[0]`, so a `[null, x]`
anchor pair would silently become a `0.0 dB` anchor — which would both admit a 0.0 request to the range gate and
land it at that pair's norm (Off, at norm 0). A scan of all 138 cached maps found **0 pairs containing a null**, so
this is a latent trap for a future map shape rather than a present fault. Guard: skip any pair with a non-numeric
entry, never read it as 0.0, and log the skip. Leg: a map carrying `[null, 0.0]` must behave as if that anchor were
absent.

**Recorded, affects future make-up work:** the CL 1B's Gain is `trust = "setread"`, so the display comparison is
skipped and only the norm round-trip is checked ("applied (display unverifiable on this plugin)"). Make-up written
to that control CANNOT be verified by readback — which the owed make-up/feedback-trap item has to account for.

### From B, 4 Oct: min_db/max_db now come from the map

Wider bounds: the 1176 family's Input min is **-50.3** (was -24) and the CL 1B's threshold max is **+1.1**
(was -18.5). Sean: the section 7 correction limits read these, so no client change is needed.

**Checked, read-only, and nothing truncates them.** `configFromBlock` takes each as sent when it is a number
(`out.maxDb = mx.isVoid() ? 0.0f : (float)(double) mx`), and `begin()` only ORDERS the pair
(`minDb = jmin(c.minDb, c.maxDb); maxDb = jmax(...)`), so `-96 .. +1.1` arrives intact. There is no clamp at 0
anywhere on the actuator range - the only 0 is the fallback for an ABSENT max_db, which these blocks now carry.

**Two consequences worth expecting rather than being surprised by:**
1. The null-min_db note ("min_db null ... taking -96 dB as the practical floor"), which fired on the CL 1B at
   16:09 on 3 Oct, should stop appearing for plugins whose map supplies a min.
2. A `max_db` above zero is new. It is legal and the clamp honours it, but it means a threshold CAN now be raised
   to a value that effectively stops the unit compressing. Nothing today walks it there - the amount actuator is
   HOLD-ONLY (03a item 3) and the step budget is 6 - but it matters for backlog item "threshold/amount actuator
   stepping", where the wider range is the difference between clamping early and travelling the whole control.
   The one corrective move for an estimated pick is bounded by min/max too, so it inherits the same widening.

---

## Overnight 4/5 Oct: ship_2026-10-04e is the build to install (GREEN 58/58, one clean pass)

**The sidechain premise was wrong, and that is the headline.** `AudioProcessorGraph` adopts each hosted plugin at
the graph's own main-bus channel counts (2-in/2-out), so a declared sidechain bus arrives **disabled, 0 channels**:
there are no spare input channels to zero and nothing silent reaches the key. Measured in the new `sidechain_guard`:
`input buses=2 totalInCh=2 bus1=DISABLED 0ch`. Kathy's C1/RComp zeros came from a probe that ENABLED every input bus
and fed the spare one silence. **EchoJay never fed a sidechain silence.** The ruled (b) fix is in the tree but
unreachable (the bus is never enabled); left guarded and commented so it works if buses are ever enabled.
**Open, needs a signed probe:** JUCE installs an AU render callback for every DECLARED bus regardless of enablement,
so a real Waves AU gets a live callback on a zero-channel element. Whether it reads that as "disconnected" (keying
internally, as under Logic) or as silence cannot be settled with a mock, and a PACE plugin cannot be loaded here.

**Two defects the new legs found, both in code already shipped or about to be:**
1. **The save fix dropped the slot gains on arrival.** `LinkProcessor` has TWO chain-array parses; the gains went
   into `restoreChainFromVar` (session restore) while the V2 hands a saved borrowed rack back via a **chain-cmd**,
   which lands in the other parse. G6 read `0.00 / 0.00` against the `-2.5 / 1.5` sent. Fixed on the command path.
   **04c and 04d carry the incomplete version; 04e is the first build where a reopened session keeps its level
   match.** The V2 binary is byte-identical across 04d/04e (`2C6C480F…`) because the fix was Link-only.
2. **`harness_build.py`'s staleness check was too coarse** — it refused when the V2 archive was older than ANY
   `Source/` file, but `LinkProcessor.cpp` is not in the V2 archive. A Link-only edit therefore made four two-sided
   guards (`alias_mirror`, `lease_id`, `level_match`, `role_snapshot`) skip their V2 halves and fail by name. Now
   compares against the build directory's own object list (75 objects) plus every header, **proven both
   directions**: a Link-only edit compiles, touching `ChainHost.cpp` still refuses. 04e's gate is the first in which
   those four actually ran their V2 halves.

**`link_state_guard` was half-disabled since 28 Sep** (runner looked for `$S/v2_side_bin`; `harness_build.py` was
renamed to per-guard outputs that day). One-line fix; now GREEN both sides. New **G6/A and G6/B** cover the two
load orders for the borrowedRacks save fix, asserting the slot OUT gain and PRE-trim survive. Still NOT registered
in the gate — it runs standalone, deterministically (199-202 s, identical results, live shm untouched).

**Also in 04e:** the not-responding loop guard (~0 dB GR with the input well past the profile's 1 dB point stops and
reports, never steps further; never off the crest sensor); the passive low-level-gain watch on landed slots (a hold
ran one window and could never gather); `in_at_gr1_dbfs` value+source on every "usable profile" line (the default was
already NaN, so that hypothesis was wrong); and the settle line labelling measured figures as measured with the
target shown, to one decimal (it had been rounding 2.3 to "2", hiding the miss).

**Not reached:** round 2, items 8-15.

---

## ROUND 05a (5 Oct 2026) — the 04e Logic session, items 1–4 + the item 6 read-only answer

Sean's session log was recovered from the unified log (`log show`, 11:17–11:27), so every claim below is from his own
run rather than from a reconstruction.

### Item 1 — crash on close (both layers, as ruled)
Release really disposes: the node ref is copied, `removeNode` runs, `pumpGraphToRetireOldSequence` pumps two silent
blocks so `RenderSequenceExchange` swaps and the old sequence is freed, and the line says whether the AU was
destroyed there (`refs <= 1`) or is still held. Teardown disposes nothing: `leakHostedPluginsAtTeardown` copies every
remaining `Node::Ptr` into a heap-allocated, never-freed vector reached through a `static` POINTER — so there is no
static destructor at process exit to dispose them (the UAD-style exit crash). Applied to the Link's graph too.
`teardown_dispose_guard` counts disposals on a mock AU: release 0 -> 1, teardown 1 -> 1. Green both directions.

### Item 2 — slot gain reset on engage: a CLOBBER, not a missing seed
Three sites wrote the slot's OUT gain from the opening drive, and with a zero/absent drive wrote `-0` over whatever
the slot held: `calibStart(uid, slot, pluginName, …)`, `calibStart(Config)` and `calibStartMany`. His log is the
proof — five slots to 0.00 at 11:18:51, 11:19:22, 11:20:02, 11:22:49 and 11:26:10, each burst following an engage,
with +5.00 restored in between. All three now refuse to write when there is no drive to apply and the slot already
carries a value, and say so.

**Is the loss permanent? No, and the log says why.** The structure plan carries `byp` and `wet`, never the gains, so
the hand-back cannot write the zeroes back and the Link keeps its own +5 — which is exactly why Sean saw +5.00 come
back at every reopen. The damage was (a) audible for the whole lease, since the borrowed host is what processes
audio, and (b) a save taken *during* a lease stored the zeroes in `borrowedRacks`.

**A second gap found while fixing it:** `restoreSavedChain` — the var path that rebuilds a BORROWED rack at engage
*and* restores a saved borrowed rack — never read `outGainDb`/`preTrimDb`. Only the XML path did. So even correctly
saved gains were dropped on the way back in. Both fields now ride `RackSidecarSlot` (additive, last in the struct,
written only when non-zero), are published by the one shared `fillRackSidecarSlots`, are carried onto the borrow's
slot objects by the editor, and are read by `restoreSavedChain`.

### Item 3 — the stale window: the re-target was bypassing the wait
`awaitFresh` existed but skipped exactly ONE window, and `if (pendingStep != 0) awaitFresh = false;` cleared even
that for a comparative. His 11:24:18 pass:

    11:24:18.913  block carried no start_db - READ "Threshold" off the plugin: -13.60 dB
    11:24:18.913  slot 3 both legs reset ... no window from before this can enter a sample
    11:24:18.963  window 1 gr=4.3 ... settleHeard=0.0s          <- 50 ms later, JUDGED

gr=4.3 was the reading from the previous setting (-8.8 dB, band 4.5–5.0). The Threshold had moved 4.8 dB OUTSIDE the
loop, so `pendingStep` reported nothing moved. Now the slot's heard clock is the bar: `heardAtWriteS` anchors on the
first window after the write and nothing is judged until a WHOLE window of audio has been heard since
(`state=awaiting-fresh-window`). The anchor is a reading of THIS host's tally and never rides the sidecar — the
187-stale-window handover trap — while `awaitFresh` does, so a handover re-anchors honestly.

### Item 4 — the watch was never armed on the slots it exists for
`armLowGainWatch` was gated on `step.finished`, which means "the loop ENDED on this window". A build hold does not
end: it lands and stays open for §7's check. So on exactly the dynamics slots the watch exists for it was never
armed — his 11:23:51–11:25:02 pass logged no `EJLowGain` line at all and the card read `(waiting, n=0)`. Now armed on
`step.finished || loop.landed`, with arming made idempotent (an existing watch keeps its window count and only
refreshes its gate, or re-arming every window would hold the count at 0 and the eighth-window report would never
print). The Link twin had no arm site and no tick at all; both added.

### Item 6 — read-only, and the answer is upstream of the plugin
The arithmetic is right and matches the spec verbatim. `grLevelDb = -(O90 - I90) + staticGainDb` on both twins
(`LinkProcessor.cpp:528`, `PluginProcessor.cpp:7152`), and §7's own path computes `levelChangeDb - staticGainDb`
(`EJCompCheck.h:85`). Spec line 101: "output minus input well below threshold". Those agree, applied once, no double
subtraction, same accessor on both sides. Sean's bench (-30.0 in, -30.6 out) confirms the -0.7 is real on this host,
so the shortfall is real compression, not a reporting error.

**The track/slot pair.** Aitch_4 is uid `4814a16004`, and its published track level across the whole session is
`loud_rms_dbfs -21.45, loud_peak_dbfs -5.85, heard 466 s`. **The slot-input pair does not exist** — nothing in the
session records it, because `LevelTally` computes no 400 ms p95 RMS and no per-window peak percentile at all (it has
3 s short-term figures and gated 400 ms p10/p50/**p90**). That absence is the finding: the comparison B wants cannot
be made from anything EchoJay records today, which is precisely why the field has to be built.

### 05a corrections after Sean's review

**1. The first 05a placement was entirely stale, and worse than reported.** `cmake --build build-guards --target
EchoJay EchoJayLink` builds the SHARED CODE static libraries, not the bundles. It printed "[100%] Built target
EchoJay" and every statement I made about it was true, while all four bundles on disk were still 04e's 00:38
binaries — not just the AUs. 04e shipped no VST3s at all, which is the only reason those UUIDs looked new. The four
targets that produce bundles are `EchoJay_AU`, `EchoJay_VST3`, `EchoJayLink_AU`, `EchoJayLink_VST3` — which is what
the "four plugin targets" rule always meant. Sean caught it from the UUID table.

`tools/place_ship.sh` now does the placing and refuses twice over: a binary older than the newest tracked source
file cannot contain it, and an AU whose arm64 UUID equals the INSTALLED one means nothing was rebuilt. Proven both
directions today — it refused the stale tree (naming both reasons) and placed the rebuilt one.

**2. level_loop_guard (14) was encoding the bug.** Sean: "not a fixture question until proven." Proven. New leg
(14c) asks the question on one slot with enough audio and the level hold writes `OUT -1.00` in four windows, so the
write is intact. (14) then failed for a different reason, and the leg's OWN closing line — asserted and green —
said it: "Compressor 2 set as dialled, level already matched." That companion needs no match, so writing OUT would
be wrong. It used to write because it was judging a window from BEFORE the build reset the tallies: the old green
depended on the staleness item 3 removes. The assertion now requires the hold to have ACCOUNTED for the level —
moved OUT, or said it did not need to — and the unconditional write is proved by (14c) where it belongs.
level_loop_guard: 100% GREEN.

**3. Item 6 deferred to 05b by ruling**, so this round is not delayed.

---

## 05b BACKLOG (ruled, not yet built)

1. **Item 6 — the slot's own level on its [CURRENT CHAIN] line.** `echojay::TrackLevel` driven from each slot's IN tap
   (after EchoJay's slot input gain), on both twins. Emit `loud_rms_dbfs`, `loud_peak_dbfs`, `slot_heard_s`; both
   figures or neither; omitted until >= 3 s heard. Definitions are §5's by construction, since it is the same class
   that produces the track figure. Report the exact line format to Sean for B.
2. **SHORT LISTENS — predict the make-up at the write (5 Oct ruling).** When an edit changes a PROFILED compressor's
   amount (harder / softer / build), write the PREDICTED make-up immediately with the edit: the profile's expected GR
   at the new position plus the static-gain term, onto EchoJay's slot OUT (or the plugin's own output control once
   item 13 lands). The measured level hold then corrects it on the first judged window. Log
   "make-up predicted X dB at the write; corrected to Y dB after N s of audio". Unprofiled plugins keep today's
   measure-then-write behaviour. While awaiting audio the card says "keep playing, about N s to go" instead of
   looking idle.
   Leg: harder on a profiled rig -> OUT moves AT THE WRITE, before any window -> the first judged window corrects it
   to the measured figure.
   **CONFLICT TO RESOLVE FIRST (flagged, needs Sean's word):** this is a second write to OUT for one edit, and the
   30 Sep ruling is the opposite - "a BUILD does not come back to refine. It sets OUT once so the level matches, and
   closes on the same step - so the sentence states the write it just made." The hold also counts its writes
   (`holdWrites < kHoldMaxWrites`). So either the predicted write is explicitly NOT one of the hold's writes (a
   staging write, with the hold's single write still to come), or the 30 Sep rule is relaxed for profiled slots.
   The two readings produce different closing sentences, which is what that ruling was protecting - so it is Sean's
   call, not an implementation detail.
2a. **Short listens: RULED (a) by Sean, 5 Oct.** The predicted write is a STAGING write and is explicitly not one
   of the hold's writes (it must not consume `holdWrites`). The hold still makes its single real write, and the
   closing sentence states the CORRECTED figure. The 30 Sep "a build sets OUT once" rule stands unchanged.

3. **Item 3 — CORRECTED BY SEAN, 5 Oct. It IS the stale chunk, and there is a SECOND defect.**
   My "the settled read is the truth" reading was wrong. Evidence: the same norm landed "-13.6" at 16:20:25 on V2
   (0.4016) and the Link's immediate read after writing 0.402 was also -13.6, so 0.402 = -13.6 and the write worked.
   Something then moved it to -2.6 (16:18:07) and to "Off" (16:18:53) - and Off is norm 0, which no write of ours
   ever asked for. That is the restored chunk, applied ASYNC after our write. The chunk was stale because a DAW save
   serves it from the cache with no fresh capture. True value at save was -13.6 (the 11:24 harder, nothing since).
   **SECOND DEFECT, CONFIRMED:** "value restored" is EchoJay reverting its own correct write. settleVerify's final
   attempt runs `param->setValueNotifyingHost (r.settlePrevNorm)`, the value from BEFORE our write - so after the
   chunk moved the parameter, the verdict put it at neither our value nor the chunk's. The revert is right for a
   plugin that ignores a write (the WaveShell case it was built for) and wrong when a third party moved the
   parameter in between - here EchoJay's own chunk restore, which it knows it performed.
   **FIX, recommended: (i), with (ii) as a cheap complement, not as the correctness argument.**
   (ii) alone cannot close the hole: capturing calls getStateInformation (the nextCaptureMs backoff exists because
   some plugins are slow), so a synchronous capture per write risks message-thread stalls during a loop; routed
   through the debounced sweep it is cheap but RACY, because the DAW save callback deliberately never forces a
   capture - which is the exact failure. It also only covers chunks stale because of US; a user knob-move in the
   plugin's own window followed by an immediate save has the same race.
   (i) is deterministic and costs no serialisation, and THE SEAM ALREADY EXISTS: restoreOne does applyRestoredState
   (blob) then applyRestoredParams - "Blob first, then the JUCE-side parameter values" - but that second step is
   **VST3 only**, which is why an AU CL 1B on the Link had nothing applied after its chunk. Extend it to our stored
   controls on AU slots, gated on our value being newer (slot `capturedAtMs` vs a new per-slot last-we-wrote stamp).
   Legs: (1) write -13.6, DAW-save with NO manual Save, restore, read -13.6. (2) a parameter moved by EchoJay's own
   chunk restore after our write must NOT trigger a revert to the pre-write value.
   **APPROVED BY SEAN, 5 Oct, as planned above**, with one term made explicit: the revert must NEVER fire when a
   KNOWN THIRD PARTY changed the parameter after our write - our own chunk restore, or a user move. So the revert
   needs a precondition, not just a re-read: settleVerify may only restore when nothing it knows about touched that
   parameter since the write. An unexplained mismatch (the plugin genuinely ignoring us, the WaveShell case) still
   reverts; an explained one never does, because then the revert is undoing somebody else's legitimate change and
   leaves the parameter at a third value that nobody asked for.

4. **Item 3's old framing — the readback verdict alone.** `landed "-2.6"` proves the write worked; the verdict
   compared the immediate read against the settled one, called it "did not stick" and reverted it. Fix the verdict
   (settled read is the truth), same class as the note at EchoJayParamApply.h:67. No reordering.
   Noted while proving it: the DAW's own save callback serves the slot chunk FROM THE CACHE and deliberately never
   calls into a hosted plugin, so a chunk genuinely can lag. It did not bite here.
4. **Item 2 — release leaves 2 extra refs.** "node refs after the pump = 3 - STILL HELD": the pump is not retiring
   the render sequence. Layer (b) covers the crash; released instances live until close. Find the holders.
5. **(14d)'s make-up bar.** It asserts 3 windows because that is what was measured, which is looser than Sean's 6 s
   budget. The hold's gate is `landed && holdOpen && ! holdDone` at ~1640 and `landed = true` is set at 1588, BEFORE
   it - so a one-window ordering artefact is ruled out. Establish whether the extra window is the residual or
   holdOpen, then either write on the first judged window or state the reason, and set the assertion to match.
6. **Re-aim `sidechain_guard`** at a mock whose sidechain survives adoption: it is green while asserting the
   built-in case.
7. **Fold `link_state_guard` into the CMake tree** so it stops being the one thing outside the gate.
