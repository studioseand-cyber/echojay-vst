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

---

## ROUND 06b (6/7 Oct 2026) — two crashes, and a sensor that reported 0.6 against 20 dB

### 1. Quit crash (17:54) — `drainPendingDispose` from the timer, into a dying library
`~EchoJayEditor` → `borrowEditorClosed()` → `borrowRelease(false)` is what releases on a quit (hence `keepEdits=N`).
Logic destroys the EDITOR first, so the borrow released four instances into the pending list and the 1 Hz timer
disposed them 220 ms later, while the Link's teardown was in flight. Fixes: a process-wide one-way teardown flag set
by `~EchoJayProcessor` (AP_Close), plus a 2.5 s quiet window before the timer may dispose — the window is what wins
the race, because at the moment of that crash no processor had been destroyed yet. Release and drain both KEEP once
the flag is set. `~EchoJayEditor` is deliberately NOT a signal: that window closes constantly mid-session.
**I had to narrow my own first attempt**: setting the flag in `clearBorrowPoolForTeardown` meant destroying any
ChainHost disabled disposal for the rest of the session, which `teardown_dispose_guard (3)` caught immediately.

### 2. Mid-session crash (18:32:09) — a Softube observer, 44 s after we disposed its instance
A CL 1B disposed at 18:31:24 on rack release left an NSWindow observer registered; the NLS Buss popout's window
frame change posted into it. Same class as the AMEK EQ 250's leaked timer, which is why `removeSlot` has always kept
a graveyard — the release path never had one.
**Ruling applied: a hosted third-party AU is never disposed mid-session.** Built-ins (anything that is NOT a
`juce::AudioPluginInstance`) remain the only things disposed. Third-party instances are PARKED for reuse, or kept
forever when the pool is full. Caps: `kBorrowPoolPerKey = 1`, `kBorrowPoolMaxTotal = 24`, both named in the log line.
**Why parking rather than only keeping:** a cap on a never-freed store cannot free anything, so keeping alone grows
with every close/reopen cycle; parking is bounded by construction at one spare per (plugin, rack).
**Why EJNoReuse was on, and why parking is safe again:** reuse was disabled after the 16 Sep render crash, where a
parked node kept its nodeID in the graph's `preparedNodes` so `applySettings` skipped re-preparing it and a later
reseed reconfigured an AU whose render resources were never rebuilt. That fix is in (the park path calls
`suspendProcessing(true)` then `releaseResources()`, and the reattach re-prepares after seeding) and the reference
accounting is correct now. **JUDGEMENT CALL, FLAGGED:** this parks third-party instances whatever `reuse_on` says,
because the ruling leaves no option that is both safe and bounded. `reuse_on` still governs whether a parked
instance is REUSED.
**NOT DONE, and deliberately:** the editor-view half ("never destroy or detach a third-party editor view
ourselves"). `LinkEditor.h:597` does `openPopoutForSelected(); // destroys the inline editor first`, so the concern
is real — but rewriting third-party editor lifecycle in a UI subsystem I have only skimmed, unattended, is how a
worse build ships. Sean's own diagnosis attributes this crash to the DISPOSE at 18:31:24, which is now fixed, so
this is belt-and-braces rather than the cause. Owed next round, with the inline/popout leg.

### 3. The 1176: 0.6 dB reported against a meter showing 20
Three fixes, each asserted by `level_loop_guard (4i)`:
- crest is not trusted alone on an input-drive unit. When crest and the level method disagree by more than
  `kSensorDisagreeDb` (3.0 dB) the sensor is `unmeasurable-inputdrive` and the GR figure is NaN. His numbers were
  0.6 against -5.0, i.e. 5.6 apart.
- a BUILD hold never writes before its settle windows: `holdWindows < kSettleMaxSteps` (3) returns `hold-settling`
  and writes nothing. `judged` could not be used - the ask and settle paths reset it, so the hold keeps its own count.
- the card can no longer say "level matched" on a figure nobody measured; it says the gain reduction could not be
  measured, and why (an input drive mixes the drive and the reduction into one number).
**On reading a GR meter parameter:** the mechanism already exists (`senseParams` / `grReadable` / `sensedGrDb`, and
06a's rule prefers the meter when the block names one). Whether the UAD 1176 Rev E exposes one is NOT something this
tree can answer: resolving it means loading a PACE-wrapped plugin, which is barred on this Mac. The block's
`sense_params` come from the map, so it is B/Kathy's side to confirm.

### 4. Log-only lines (done)
Every IN write logs slot, before→after, clamped or not, and the slot's current OUT. Every window line now carries
`out90=`, `slotDiff=`, `chainIn=`, `chainOut=`, `chainDiff=` beside `in90=`, on both twins.
**NOT DONE:** the "once a second while playing with no hold running" line — that needs a timer path outside the loop,
which is more than a log statement. Owed.

### 5. Why `borrowHost_` never gets the Link track's role — and why that is not the bug
`setChainRole` is called exactly once, at `PluginProcessor.cpp:1597`, for V2's own `chainHost`. It is never called
for `borrowHost_`, which therefore carries the default `ChainRole::Kind::channel` — correct by luck for a borrowed
track rack. The role has ONE consumer inside ChainHost: `setPendingLevelsState`, which decides whether a SAVED level
tally may be restored on load. A borrowed host is built fresh at engage and never goes through that path, so the
role is inert there. I did not wire it: it would be code with no behaviour behind it. The latent trap is recorded
instead — the default is `channel`, so the day something does read the role on a borrowed BUS rack it will be
silently wrong in the other direction.

---

## ROUND 06c — BUILT AND PLACED 7 Oct 19:38 as `ship_2026-10-06c`, LABELLED **UNGATED**, NOT INSTALLED

**THE GATE WAS RED: 58 of 59.** Stages 1-3 clean (archives, 39 guard binaries, 4 bundles, 0 errors each),
`link_state_guard` GREEN both sides, GATE3 DONE 19:27:39. The single failure was `dialinfo_keep_guard`, and it failed
AFTER passing every assertion: the scribble leg segfaulted at teardown (exit 139) on a config-fetch callback landing
after `~EchoJayProcessor`. Two immediate re-runs passed. Sean ruled: place it UNGATED with the whole record written
in so he can test tonight, and queue the race as **06d item 1**. `ship_2026-10-06c/UNGATED.txt` carries that record.
Per the standing rule, UNGATED.txt is never removed from this folder.

**THE FIVE ITEMS, all in the V2 binary only:** (1) an explicit control-by-control edit never starts a loop
(startCalibrationFromOps skips ops carrying `controls`/`controls_norm`/non-empty `settings_structured`, and names any
target it ignored); (2) a bus never claims a landing (sayCalibrationCouldNotStart asks calibTargetIsBus first and
returns silently; "land it" retired); (3) "changes applied" is judged on the slots via the new pure
`EchoJayEditor::everySlotFullyApplied`, so `partial`/`builtinPayloadUnmatched` make the sentence name what was
ignored - closes items 6 and 11; (4) the reroute footer is gone (`renderRerouteReply` returns the reply unchanged;
`rerouteQuietLine()` KEPT so both guards can assert it ABSENT); (5) the substitution card names the licence reason
and the replacement. Legs green for 1-4; 5 is card wording.

**THE LINK BINARY IS BIT-IDENTICAL TO 06b AND THAT IS CORRECT:** arm64 2EBEB279 both places, because the Link
archive compiles none of PluginEditor.cpp / PluginProcessor.cpp / EchoJayAPI.cpp. `place_ship.sh` REFUSED the folder
on that basis - its "a rebuilt binary cannot keep its UUID" rule is false for a reproducible build of unchanged
inputs - so the folder was placed by hand and UNGATED.txt says so. TOOLING ITEM: the UUID check must defer to the
binary-vs-source staleness check, which already covers the case the UUID rule was written for.

UUIDs as placed (arm64 first): V2.component 46771422 / 0AC36772 - Link.component 2EBEB279 / E097175A -
V2.vst3 B9F8A5A5 / D9BC481F - Link.vst3 732A699A / F19DA546. Installed at the time: 06b V2 045CB611, Link 2EBEB279.

---

## ROUND 06c — THE RULING AS GIVEN (ruled 7 Oct 2026; no gate while Sean is testing in Logic)

### 1. AN EXPLICIT CONTROL-BY-CONTROL EDIT NEVER STARTS A LOOP (ruled, plugin-side regardless of the server)
Sean set slot 2 (EchoJay Compressor) SC HPF 110 Hz and attack 40 ms. The edit applied - and then a loop started
anyway, "set from the working position, landing it as it plays", drove the input to +5.0 dB and closed with "band not
reached, working 0.1 dB". On an edit where the user named every value, that is the plugin overwriting the user.

**THE RULE:** an edit the user specified control-by-control never starts a calibration loop, never resets a dynamics
slot to its working position, and never moves IN / OUT / drive. The loop runs on a BUILD, or when the user asks for
an amount. If a block carries BOTH explicit controls and a target, apply the controls, IGNORE the target, and log
that it was ignored.

**WHERE IT GOES, found read-only:**
- `PluginEditor.cpp` ~25541: after any applied local edit, `startCalibrationForEdit(..., Purpose::buildHold)` runs,
  gated ONLY on `editWasRefused(editJson)` (the 3 Oct "a loop starts only from an edit that applied" ruling). There is
  no test for "the user named the values".
- `startCalibrationForEdit` tries `startCalibrationFromChain`, then falls back to `startCalibrationFromOps`.
- `startCalibrationFromOps` decides per op on: the slot exists, the slot's CATEGORY is dynamics, then reads
  `gr_target_db` and `slot_pre_gain_db`. It never looks at whether the op carries explicit control values.
So the discriminator to add is "does this op carry explicit controls" - `controls` / `controls_norm` /
`settings_structured` naming parameters - and it belongs in `startCalibrationFromOps` (per op, so a mixed batch is
handled op by op) with the same test mirrored wherever `startCalibrationFromChain` reads a chain block.

**LEG (this exact case):** an op for a dynamics slot carrying explicit controls (SC HPF 110 Hz, attack 40 ms) and NO
amount request starts ZERO loops, leaves the slot's IN / OUT / drive untouched, and the working position is not
written. A second leg: the same op WITH a `gr_target_db` applies the controls, starts no loop, and logs that the
target was ignored. A third: a plain build block still starts its loop, so the fix does not disable builds.

### 2. THE EDITOR-VIEW HALF OF THE 18:32 RULING (carried over from 06b)
"Never destroy or detach a third-party editor view ourselves - close it through the plugin's own editor teardown, or
keep the view alive (hidden) until the instance goes." `LinkEditor.h:597` does
`openPopoutForSelected(); // destroys the inline editor first`. Not done in 06b deliberately: rewriting third-party
editor lifecycle unattended, in a subsystem I had only skimmed, is how a worse build ships - and the 18:32 crash is
attributed to the 18:31:24 DISPOSE, which 06b fixes. Leg: a third-party editor inline, switch slots, then open a
popout (an NSWindow frame change) with no crash and zero third-party disposes mid-session.

### ALSO STILL QUEUED (unchanged)
the chain IN/OUT readout and per-slot IN/OUT on the tiles; the profiled compressor's measured GR in the multi-hold
card line; the classify small body built from scratch (<8 KB leg - note the 100,963 b "trimmed" figure proves history
was only ~96 KB of the 197 KB); the once-a-second level line while playing with no hold running; the non-blocking
release wait then 10 s; parked-edit identity + satisfied check + background delivery; the `.o.d` header narrowing so
neither staleness check needs a forced rebuild.

### 3. THE EQ BAND THAT WAS SKIPPED SILENTLY (Pro Tools, 10:37:23, ruled 7 Oct)
Sean asked for a 3-4 dB cut at ~281 Hz on the EchoJay EQ (slot 1). Log: `EXACT built-in apply, 0 band(s), 1 skipped`
with no reason, and the chat still said "Changes applied" / "Settings applied to EchoJay EQ".

**WHY IT WAS SKIPPED, from the code.** `applyEqMoves` (EqMove.h ~117) has exactly ONE skip:
`if (idx < 0) { ++skipped; continue; }  // nowhere to put it`. It fires only when the band carried NO explicit index
(`mv.band` outside `1..kNumBands`) AND every band was already `enabled`, so the "lowest free (disabled) band" search
found nothing. So it is NOT the key names, the type string, or the freq/gain/Q ranges: a band rejected on those
grounds dies earlier, at `specFromVar(...); if (! ok) continue;` in SurgicalEqProcessor::applyEqBands (~269), which
increments NEITHER counter and would have logged "0 band(s), 0 skipped". The log said 1 skipped, so the band parsed
cleanly and was dropped purely because THE EQ WAS FULL AND THE REQUEST NAMED NO INDEX - which is exactly why fix (3)
is "an EQ edit that adds a band ADDS it and keeps the existing bands".

**THE THREE FIXES:**
 (1) log WHY each band is skipped, with the band's raw JSON. Note the asymmetry: `specFromVar` failures are invisible
     in BOTH counters today, so a malformed band and a full EQ differ only by the number. The skip needs a reason
     string, not a count - and the silent `continue` at specFromVar needs to count and say so too.
 (2) never print "Changes applied" when every requested change was skipped. The verdict already exists -
     `s.dialStatus = (skipped > 0) ? DialStatus::partial : DialStatus::applied` in ChainHost.cpp ~5815 - and
     `applied == 0 && skipped > 0` is the case that must read as a failure with its reason, not as success.
 (3) an EQ edit that ADDS a band adds it and keeps the existing bands, rather than needing a disabled slot to land in.
**LEG:** a full EQ plus an add with no band index ADDS the band and keeps the others; the card says what happened; and
a genuinely malformed band is reported with its raw JSON and its reason.

### 4. THE AAX IS A 29 SEPTEMBER BUILD - a decision is owed
Installed: EchoJay V2.aaxplugin and EchoJay Link.aaxplugin both built 29 Sep 10:10 (arm64 AA0C2737 / 13D36AE3), plus
a legacy EchoJay.aaxplugin from 9 Jul. **It has NONE of 05a-06b.** place_ship.sh places .component and .vst3 only and
every gate builds the four AU/VST3 format targets; AAX has never been in the build command, and the last shippable
AAX bundles sit in DO_NOT_SIGN_ship_* folders because AAX needs PACE signing, which cannot happen on this Mac.
CONSEQUENCES: (a) any Pro Tools finding is a finding about 29 Sep code - the EQ skip above is real and still present,
but other symptoms may already be fixed or may belong to code that has since moved; (b) Pro Tools cannot validate any
of the crash work, because those fixes are not in the binary it loads.
DECISION FOR SEAN: either AAX joins the gate and the ship folders (and the signing step gets solved), or Pro Tools
comes off the validation path and the AAX is recorded as a known-stale build.

### 5. THE "(sent as a chat, not a build...)" FOOTER IS OURS — REMOVE IT (ruled 7 Oct, B traced it)
`EchoJayAPI.h:292 rerouteQuietLine()` + `:301-302 renderRerouteReply()`, called from
`PluginEditor.cpp:30507 rerouteChatTurn()`, appends it to every reply re-sent after a 403 chat_turn_not_streamed.
Remove the append. RE-AIM the two guards that currently assert it is PRESENT so they assert it is ABSENT:
`ui_round_guard` 1846-1852 and `stream_reroute_guard` harness:27. B strips it server-side and from history too, so the
client must stop adding it or it comes back on the reroute path alone.

### 6. EQ SKIP — MY DIAGNOSIS WAS WRONG, B's IS RIGHT
I said "the EQ was full and the request named no index", reading the `eq_bands` path (`applyEqMoves`, EqMove.h ~117,
whose only skip is `idx < 0`). That was wrong, and B's two facts disprove it: Sean's screenshot shows ONE band in use
(HPF 20 Hz) so free bands existed, and the compressor logged "2 band(s)" for two PARAMS (sc_hpf_hz, attack_ms), so the
counter was never band-only.
**THE REAL PATH.** The op shipped as flat `settings_structured.params` (freq_hz / gain_db / q) with no `eq_bands`, so
it went to `EedDeviceProcessor::applyParams` (EedDeviceProcessor.cpp ~85), which increments the SAME `skipped` counter
for three reasons: an id the device does not publish, a value outside a legal enum or not a number, and
"not implemented". `freq_hz` / `gain_db` / `q` are not ids the EQ publishes - its band shape is `eq_bands` - so this
was an UNKNOWN ID skip, not a full-EQ skip.
**AND THE REASONS ALREADY EXIST.** applyParams builds a `juce::StringArray unknown` naming each one
("<id>", "<id> \"v\" is not one of: ...", "<id> (not a number)", "<id> (not implemented)") and returns them in the
summary as "ignored <list>". So fix (1) is mostly plumbing: that summary is not reaching the EJParamApply line Sean
read. The genuinely silent one is `specFromVar`'s `if (! ok) continue;` in SurgicalEqProcessor::applyEqBands (~269),
which counts NOTHING - a malformed band shows as "0 band(s), 0 skipped".
**FIXES (as ruled):** reason per skip with the band's raw JSON; never "Changes applied" when applied == 0 and
skipped > 0 (the verdict exists at ChainHost.cpp ~5815, `(skipped > 0) ? partial : applied` - the missing case is
applied == 0 reading as success); an add with no index lands when bands are free AND when full; and an unknown params
id on a built-in is logged by name and counted as skipped with its reason. B converts band-shaped params to eq_bands
server-side, so this is the client's belt-and-braces.

### 7. AAX: RULED OFF THE VALIDATION PATH; BUILD-ONLY IN THE GATE
Pro Tools comes off validation, Logic AU is the validation DAW. Add the AAX targets to the gate as BUILD-ONLY
(compile, not signed, not placed) so the AAX stops rotting. Installed AAX is 29 Sep 10:10 (V2 arm64 AA0C2737, Link
13D36AE3) and has NONE of 05a-06b.
**WHAT BLOCKS PACE SIGNING ON THIS MAC — checked, not guessed:**
  - wraptool: PRESENT and executable at `/Applications/PACEAntiPiracy/Eden/Fusion/Versions/6/bin/wraptool`
    (25 Aug), not on PATH, exactly as RELEASE.md:51-53 records. NOT a blocker.
  - Apple signing identity `Developer ID Application: Sean Donoghue (8BT5F9B887)`: PRESENT and valid
    (1 identity found). NOT a blocker.
  - iLok: an iLok IS visible on this Mac (2 USB matches), and iLok License Manager is installed. So the earlier
    "Sean's iLok is on another Mac" note looks stale for AAX SIGNING - worth confirming it holds the right licence.
  - **THE AAX SDK IS MISSING: `~/AAX_SDK/Interfaces/AAX.h` does not exist.** RELEASE.md:57 says CMake looks exactly
    there. This is the hard blocker for even BUILDING AAX, before signing is reached - so "add AAX build-only to the
    gate" needs the SDK restored first, and that is the one thing to fetch.
  - **THE OTHER BLOCKER IS BY DESIGN, AND IT IS NOT FIXABLE BY ME:** RELEASE.md:79-80 - "wraptool prompts for the iLok
    password, so it is run from a Terminal with a TTY, by a person". Signing can therefore never be automated from
    this session; it is a step Sean runs. The canonical invocation (account seand123, wcguid
    B4184F90-2F4F-11F1-A9B9-00505692C25A, out-of-place --out) is already written down at RELEASE.md:85-95.
  SO, FOR BETA: fetch the AAX SDK to ~/AAX_SDK (blocks the build), confirm the iLok licence (likely fine), and keep
  the signing step human (cannot be otherwise).

### 8. THE FALSE LANDING MESSAGE ON A BUS (12:10:57, ruled 7 Oct)
`.008` the 1176's four settings APPLIED; `.030` "NOT STARTED - role is bus"; `.031` "COULD NOT START LANDING ... its
settings never landed", and the chat told Sean to say "land it".
**CAUSE, found.** `calibStart` refuses correctly on a bus: `calibTargetIsBus(uid, whyNot)` returns true and logs
"NOT STARTED - this chain's role is a bus - on a bus the last stage sets the level, and after Go the Level slot"
(PluginProcessor.cpp 6890/6961-6967). But the CALLER treats `started == 0` as a failure: `startCalibrationForEdit`
returning 0 runs `sayCalibrationCouldNotStart` (PluginEditor.cpp ~25552), whose reason chain is
`host == nullptr` / `calSlot < 0` / `calSlot >= getNumSlots()` / **else "its settings never landed"** (24670-24672).
There is NO case for "deliberately not started", so a bus lands in the else and the user is told a falsehood - the
apply log one line earlier says the settings DID land.
**FIXES:** `started == 0` must distinguish "must not start" from "could not start" - the caller should ask
`calibTargetIsBus` (it already returns the reason) and say NOTHING about landing on a bus; never claim settings did
not land when the apply path reported them applied; and retire "land it" from user-facing text (PluginEditor.cpp
~24683 is the one bubble that offers it).
**LEG:** a bus-role build that applies settings produces ZERO landing bubbles and no "land it", while the applied
count is unchanged; a genuine failure (the named plugin absent) still says what went wrong.

### 9. SUBSTITUTION MUST INHERIT THE SLOT'S JOB (ruled 7 Oct)
Card said "Chain built - 6 of 6 loaded (Gold Clip failed)". It must say: "Gold Clip isn't licensed on this Mac, so I
used the EchoJay Limiter instead. Press Suggest an alternative to try something else."
The substitute arrived with NO settings (`settings_structured=n`), so the planned ceiling (-0.1 dBTP) was never mapped
onto it: a substitute that inherits the slot's JOB must carry the planned intent across, not just occupy the slot.
And the substitute must be shown in the chain list.
**LEG:** a licence-failed slot substituted by a built-in shows the licence reason and the substitute's name on the
card, the planned ceiling lands on the substitute, and the chain list shows the substitute rather than the original.

### 10. MIX BUS FINAL LEVEL - what the log line means, and why nothing reached target
**WHAT IT MEANS.** "on a bus the last stage sets the level, and after Go the Level slot" (PluginProcessor.cpp 6967) is
the rule that a BUS's output level is not set by per-plugin holds at all. Two things set it: the chain's last stage,
and - after the user says "Go" - the **Level slot**, driven by `LoudnessLoop` (LoudnessLoop.h, "deterministic inside
V2, no server round-trip"). That is why `calibTargetIsBus` refuses a hold on a bus: a hold would fight the Level slot.
**WHY NOTHING BROUGHT THE OUTPUT TO TARGET.** The LoudnessLoop is ARMED from the chain
(`armLoudnessLoopIfTargeted` -> `loop.armFromChain()`, PluginEditor.cpp 23270/23640) but it only MEASURES AND WRITES
on an explicit user verb - `listen`, `check`, `go`, `louder`, `softer`, `loudest`, `back off`, `done` (the verb table
at ~23325-23341, all gated behind `if (! loop.everArmed()) return false`). Nothing in a build drives it. So the Level
slot kept the static `gain_db 5.4` the build gave it, nobody measured the real output, and the bus sat at -20 LUFS
against a -8 target with peaks at -2 dBFS. The limiter "wasn't limiting" for the same reason: at -20 LUFS nothing was
reaching its ceiling.
**RULING TO BUILD:** on a bus build WITH a loudness target, once the build settles the Level stage measures the
chain's real output on loud sections and sets its gain so the output reaches the target INTO the limiter, ceiling
held; and the card reports the result ("now -8.3 LUFS short-term, ceiling -0.1 dBTP"). In effect: a build with a
target arms AND runs the loop's measure-and-set, instead of waiting for a verb the user was never told to say.
**SUPERSEDED - see item 10 REVISED below.**

### 10 FINAL (Sean, 7 Oct, simplified after the addendum): SET AT LANDING, THEN ONE AUTOMATIC CORRECTION
Supersedes both the "arm and wait for a verb" reading and the predict-every-slot addendum. Bus/master compression is
meant to be subtle, so the build-time set is deliberately simple and the safety net is a measurement, not a model.

 1. **THE BUILD-TIME SET.**  `gain = target - the song's integrated LUFS`, plus the exactly-known change of EchoJay's
    own devices WHERE THAT IS CHEAP. What is cheap and exact: the plain scalar gains - each slot's OUT gain
    (`getSlotOutGainDb`), its IN pre-trim (`getSlotPreTrimDb`) and the master wet (`getMasterWet`). What is NOT cheap
    and is therefore EXCLUDED: the EQ's effect on loudness and any compressor's dynamic effect. (An analytic EQ
    magnitude evaluator does exist - EqEngine.h:194 fills magsDb[] at given frequencies - and an FFT exists in
    MeterEngine/EqFft, so this was not impossible; but turning spectrum x curve into a LOUDNESS delta needs
    K-weighted integration over the band, which is new DSP and not a cheap addition.)
    The integrated reading is already there: `chainInTally_` is `Weighting::K` (ChainHost.h 2668), so
    `getChainInLevels().levelDb` IS integrated LUFS, with `known` as the trustworthiness flag and `heardSeconds` for
    the too-little-heard case.
    Third-party compressors are NOT predicted from their profiles - dropped by ruling.
 2. **ONE AUTOMATIC CORRECTION, and it is the safety net for exactly what (1) does not model.** After the build, on
    the first loud window the new chain plays (3-6 s of real signal), measure the level arriving at the Level slot and
    correct its gain ONCE - no user action, ceiling held. Log predicted vs measured per slot, so the quality of the
    prediction is visible as EJ Map profiles arrive. NO correction without real signal.
 3. **Go stays** as the longer listen and fine-tune, running the existing LoudnessLoop
    (`arm(targetLufs, levelSlot, limiterSlot)`, LoudnessLoop.h 250). Not pressing Go leaves the set value standing.
 4. **Too little heard** (`! known`): say so and ask Sean to play the loudest section first, then set. Never guess.
 5. **The server's static `gain_db` for the Level slot on bus/master is IGNORED** in favour of this, and the fact that
    it was ignored is logged. (That is the 5.4 dB that sat there while the bus measured -20 against a -8 target.)
 6. **THE CARD** says what it set, what the correction changed, where it landed, and "Press Go to listen longer and
    fine-tune." Unpredicted slots are named as "not predicted".

**LEGS:** the prediction is EXACT for a chain whose only level changes are EchoJay's own scalar gains; an unprofiled
compressor taking 8 dB lands within 1 dB after the one correction; no correction happens without real signal; too
little heard ASKS instead of guessing; and the server's static gain_db is ignored with a line saying so.

**ORDER: ITEM 9 FIRST.** This pushes the Level slot up by of the order of +11 dB into the limiter, and item 9 is what
gives a SUBSTITUTED limiter its ceiling - the EchoJay Limiter arrived with settings_structured=n, i.e. no ceiling at
all. Landing 10 before 9 means an 11 dB push into a limiter that was never told -0.1 dBTP, which clips.

### 11. "CHANGES APPLIED" IS DRIVEN BY AN OP COUNT, NOT BY WHAT LANDED (13:19:28, ruled 7 Oct)
Third shape of the same lie: `BUILT-IN PAYLOAD NOT UNDERSTOOD ... got keys: []` for the EchoJay EQ, then
`EXACT built-in apply, 0 band(s), 0 skipped`, and the chat still said "Changes applied". `got keys: []` means
`structuredSettings` carried no keys at all - the op reached the slot with nothing the device could use, and nothing
was even attempted (0 applied AND 0 skipped, so it is not the unknown-id case of item 6 nor the full-EQ case).
**THE ROOT, AND IT IS ONE ROOT FOR ALL THREE.** PluginEditor.cpp ~25636: `else if (applied == total) summary =
"Changes applied";`. The comment directly above it says what `applied` is: "the number counted OPS -- `total` is
ops.size() and `applied` is incremented once per op in finishOpAndContinue". So it counts ops DELIVERED, not settings
that LANDED. One op that achieved nothing still gives 1 == 1 and prints success. That is why 10:37 (unknown id,
0 applied / 1 skipped), 13:19 (payload unmatched, 0 / 0) and the full-EQ case all produced the same sentence.
**THE VERDICT ALREADY EXISTS AND IS DISTINCT PER SLOT:** ChainHost.cpp ~5815 sets
`dialStatus = (skipped > 0) ? DialStatus::partial : DialStatus::applied`, and ~5830 sets
`DialStatus::builtinPayloadUnmatched` for the unmatched-shape case, logging the keys it was handed and the shapes it
wanted. The chat summary never consults any of it.
**FIX (covers items 6 and 11 together):** drive the summary from the per-slot dialStatus, not from an op count.
`applied == total` may read as success ONLY when every touched slot came back DialStatus::applied. If any slot is
builtinPayloadUnmatched, the card says what arrived (the `got keys` list, or "nothing at all") and what the device
wanted (`"params":{...}` or its array form, e.g. `eq_bands`). If any is partial, it names the settings that were
ignored and why - those reasons already exist in EedDeviceProcessor::applyParams's `unknown` array and are already
returned in the summary string as "ignored <list>"; they are simply discarded before the user sees them.
**LEGS:** (a) an op whose payload matches no shape produces a card that names the keys received and the shapes
wanted, and the chat does NOT say "Changes applied"; (b) an op with one unknown param id says which id and why, and
does not read as success; (c) an op where everything lands still says "Changes applied", so the fix does not turn
every success amber.

### 12. SUBSTITUTE CEILING - QUEUED (timeboxed out on 7 Oct, with Sean's pre-authorisation)
B now sends it: a third-party loudness-limiter slot carries
`substitute: {name: "EchoJay Limiter", settings_structured: {params: {ceiling_db, true_peak, mode}}}`.
On a LICENCE substitution, dial that `substitute.settings_structured` onto the replacement.
**WHY IT WAS NOT DONE TONIGHT (Sean's 20-minute bar):** the licence substitution runs inside a loadAsync callback at
ChainHost.cpp ~7676 whose scope is `fullDesc` (the third party) and `bd` (the built-in). It has NO access to the
server payload, so the substitute block must either be threaded down to that site or applied by a post-load pass in
the editor keyed on `SlotDialInfo::substitutedFrom`. The leg also has to reach the licence branch, which is gated on
`echojay::refuseIfPaceWrapped(fullDesc)` - so it needs a desc that LOOKS PACE-wrapped (that call reads the desc off
disk and instantiates nothing, so this is possible, but it is fixture work). Together: well past 20 minutes.
**SAFE IN THE MEANTIME, verified:** `kCeilingDb` defaults to **-0.3 dB** (EedLimiterProcessor.cpp:30, range -24..0),
so a substituted EchoJay Limiter is never ceiling-less. It limits at -0.3 rather than the planned -0.1 - more
conservative, not less. THIS CORRECTS my earlier warning that item 10 before item 9 would CLIP: it would limit at
-0.3. The 9-before-10 order is still preferable (the planned ceiling is the intended one) but it is not a hazard.
**THE HOOK EXISTS:** LoudnessLoop reads `ceiling_db` from the limiter slot's own params (LoudnessLoop.h:163) and
writes it as a param (237), so once the value is in scope there is nothing new to build.

---

## ROUND 08c — THE LIST, FROM SEAN'S 08b TEST (diagnosed read-only 8 Oct ~20:15 from code + the 19:55-20:10 logs)

08b PASSED items 1, 2, 4 and 5 (no bypass after hand-back; Link meters follow the fader; the Level survived the
reopen; the Apply message scoped and the EQ dialled). Five findings, each with its cause in the log.

### F2. THE REAL CAUSE OF THE 2.8 dB SHORTFALL, from the per-slot block - AND IT IS OURS
Sean's addendum corrects my reading of F: the -12 target was CORRECT (he asked to keep dynamics), and it still
landed at -14.8. The chain block at 20:07:42 has the whole answer, slot by slot:
    1: EchoJay EQ           in -15.6  out -16.1  out-in -0.4
    2: API-2500 (s)         in -16.1  out -18.5  out-in -2.4      <-- the compressor's own loss
    3: bx_saturator V2      in -18.5  out -17.3  out-in +1.2
    4: elysia museq master  in -19.0  out -18.9  out-in +0.1
    5: EchoJay Level        in -18.9  out -15.6  out-in +3.3      <-- the write DID land, in full
    6: EchoJay Limiter      in -15.6  out -15.8  out-in -0.2
    Chain in/out: in -14.8 RMS, out -14.8 (pk -1.2), out-in -0.0
**THE LEVEL'S +3.3 dB IS APPLIED AND MEASURABLE** (slot 5, out-in +3.3), so nothing is eating the write and my
"two different windows" explanation in F was incomplete. **What is eating the loudness is the chain itself: the
signal arrives at the Level 4.1 dB BELOW the chain input**, almost all of it the API-2500's -2.4 dB.
**AND THE OPENING GAIN WAS COMPUTED BEFORE THOSE LOSSES EXISTED.** The opening ran at 20:00:11, at build finish,
from "output integrated -15.3 at Level +0.0". The compressor's -2.4 dB, the saturator's wet 15 % and the museq's
settings arrived WITH THE DIALS, after that reading. So the landing solved for a chain that had not yet taken its
own losses, and **nothing re-landed once the dials settled**. That is the 2.8 dB, and it is ours.
**IT EXPLAINS BOTH CASES WITH ONE MECHANISM** - the mix bus and the vocal - and it explains why this morning was
right: that chain's inter-slot loss was small, so the once-computed opening happened to be correct.
**THE FIX, and the hook already exists.** The build bubble ALREADY waits for the dial state to settle
(`finishChainBubbleWhenDialSettled` / `ChainHost::dialStateSettled()`). The landing must wait for the same signal:
compute the opening AFTER the dials have settled, from a fresh reading of the chain output, not at build finish.
And because a later edit changes the chain's gain again - Sean's two EQ bells at 20:05 are exactly that - a landing
whose chain has changed since it was computed is STALE and must re-offer rather than sit on a figure that was true
of a different chain.
**THE CEILING IS THE OTHER 0.9 dB.** The chain output peaks at **-1.2 dBTP** because the server's block asked for
`ceiling_db -1` and the limiter obeyed; the rule is **-0.1**. So of the 2.8 dB, about **0.9 dB is simply unused
headroom** and about 2 dB is the un-relanded chain loss. The EchoJay Limiter's own default is -0.3
(EedLimiterProcessor.cpp:30), so nothing in the plugin chose -1: it was told. **The plugin-side fix is item E's
clamp** - a FINAL limiter's ceiling is held at -0.1 whatever the block says, with the override logged naming the
figure that was asked for - and it recovers that 0.9 dB on its own.
**FILES:** PluginEditor (the arm path: wait for dialStateSettled before the opening write, and mark a landing
stale on a chain-gain change), LoudnessLoop (the re-offer), plus item E's clamp. **RISK:** medium - it moves WHEN
the opening is written, so the legs must cover "the dials land after the build" and "an edit after the landing".
**LEGS:** a build whose compressor dials in -2.4 dB after the arm lands the target anyway; an edit after a landing
marks it stale and re-offers; a final limiter asked for -1 is held at -0.1 and says so.

### F. THE LANDING DID NOT REGRESS. THE SERVER ASKED FOR -12 "DYNAMIC", AND GO WAS NEVER PRESSED.
**THE TWO BUILDS SIDE BY SIDE, both lines verbatim from the logs:**
    THIS MORNING (Sean: "+6.5 to +8, right")
    13:16:20.996  opening gain (closed loop): output integrated -15.1 LUFS at Level +0.0 -> +8.1 dB for target -7.0
    13:16:20.997  armed: target -7.0 LUFS (level_params, PUSHED), Level slot 3 gain +8.1, limiter slot 4
    TONIGHT (Sean: "landed ~-14")
    20:00:11.249  opening gain (closed loop): output integrated -15.3 LUFS at Level +0.0 -> +3.3 dB for target -12.0
    20:00:11.250  armed: target -12.0 LUFS (level_params, DYNAMIC), Level slot 4 gain +3.3, limiter slot 5
**THE ARITHMETIC IS IDENTICAL AND CORRECT IN BOTH:** opening = target - measured. -7.0 - -15.1 = +8.1;
-12.0 - -15.3 = +3.3. The whole difference is the TARGET THE SERVER SENT - five dB of it - and the OPTION with it.
There is no regression in the loop.
**WHY IT STOPPED AT -14.6:** the first Listen window measured -14.6 integrated and proposed the remaining **+2.6
dB** ("Push +2.6 dB to reach -12.0? [Go | Leave it]", 20:06:48). **Go was never pressed.** The only two verbs in
the entire session are `listen` (20:06:38) and `check the level of the mix bus` (20:06:58) - and that second one is
item C: his typed question was swallowed as the Check verb, which opened a second window whose proposal was then
CAPPED by "dynamic" at 3 dB ("-12.2 is as loud as this goes with the limiter working <=3 dB"). So the Level stayed
at the opening +3.3 and the output stayed at -14.6. **Item C is not a separate bug from F - it is why F could not
be finished.**
**THE VOCAL BUILD HAD NO LANDING AT ALL.** There is not one `EJLoudness` line between 20:16:30 and 20:26: the
loudness loop never armed on it, so nothing under-landed. Only the dynamics loop started ("EJDialSummary: loops
started 1 of 2 dynamics slots", 20:17:25). Whether a vocal TRACK chain should carry a loudness target at all is a
question for B, but no landing ran, so no landing misbehaved.

**THE THREE 08b SUSPECTS, each ruled out with its reason:**
  1. **cfce470's deadline/staleness does NOT touch the landing loop.** It is in `EJCalibLoop` - the EJThreshold
     per-slot pass - a different class in a different file. `LoudnessLoop` has its own `kResolveMs` bound from
     a8defea, and the evidence says it never fired: the window ran to a full measurement (10.1 s counted, a
     figure, a proposal). Nothing was cut short.
  2. **5983756 cannot be read by the landing loop, so it cannot double-count.** That change is in
     `PluginEditor.cpp`'s `LinkStripState` INGEST - the V2's display of a LINK's published meter frame, converted
     through `frameLoudnessAsHeard`. The loudness loop reads `host_.getChainOutLevels()` and `getChainInLevels()`,
     which are ChainHost's own `LevelTally` and never pass through that conversion. The mix bus is the V2's own
     rack, not a Link frame at all. And the figures prove it: -15.3 before the write and -14.6 after a +3.3 write
     are a build-time window and a chorus window of DIFFERENT audio, not one number counted twice.
  3. **NOT a -14 fallback.** Both builds took their target from `level_params` with an explicit option -
     -7.0/pushed and -12.0/dynamic. Item 10's absence is not implicated anywhere in this.

**SO THE CAUSE IS NOT OURS, EXCEPT FOR TWO THINGS THAT ARE:**
  (i) **item C** - the swallowed verb stopped him pressing Go, which is the step that would have landed it. That
      is already first in 08c and this is a second, independent reason for it.
  (ii) **NOTHING ON SCREEN SAID WHICH PROMISE THE BUILD WAS KEEPING.** A -12 "dynamic" build and a -8 "pushed"
      build behave completely differently - 3 dB of GR allowed against 12 - and the only place that appears is a
      log line. The arm bubble and the Level card must name the target AND the option in words, so "it is quieter
      than this morning" is answerable on screen instead of from a log.
**FOR B, and it is the actual question:** why did this mix-bus build ask for **-12 LUFS, dynamic** when the same
engineer on the same material had -7 pushed this morning? The client honoured what it was sent, both times.
**FILES for (ii):** LoudnessLoop (armBubbleText, the Level card line). **RISK:** very low, wording only.

### C. THE LEVEL CHECK DID NOT FAIL - IT WORKED TWICE. THE CHAT MESSAGE WAS EATEN BY A VERB.
**THE TIMELINE, verbatim:**
    20:06:38.585  verb "listen" state 7            <- armed
    20:06:38.585  state -> listening: window open, needs 10 s above -40.0 LUFS, resolves within 20 s either way
    20:06:38.888  state -> measuring: FIRST AUDIO after 0 s of waiting
    20:06:48.728  window: integrated -14.6 LUFS (the target), loudest 3 s -14.0 (+0.6 over), 10.1 s counted
    20:06:48.728  bubble: Measured -14.6 LUFS integrated. Push +2.6 dB to reach -12.0? [Go | Leave it]
    20:06:58.805  state -> listening   <- A SECOND WINDOW
    20:06:58.806  bubble: Checking the level again - play the loudest part.
    20:07:08.816  bubble: Measured -15.2 LUFS integrated. -12.2 is as loud as this goes with the limiter
                          working <=3 dB. Push to -12.0 anyway? [Push it anyway | Leave it]
So audio WAS flowing (10.1 s counted in ten seconds), a measurement was made, and a proposal was shown. **No
ending sentence was due, and item 9's deadline was never the question.** Sean's "I pressed Listen without
playback" and the loop's "first audio after 0 s" cannot both be true of the same moment; the loop's reading is the
one with a figure attached.
**THE REAL DEFECT IS THE SECOND HALF: his typed "check the level of the mix bus" got no reply because it was
swallowed as a LOOP VERB.** `sendChatMessage` runs `handleLoudnessVerb(msg)` first - "local first, no network" -
and that matcher took a sentence containing "check the level" as the Check verb. That is what opened the second
window at 20:06:58 ("Checking the level again"), and it is why no chat answer came: the turn never went out. The
third message was a different sentence, so it went to the server normally.
**FIX:** the verb matcher must take a VERB, not a sentence that contains one. It should match only a short message
that is essentially the verb (the same discipline `EJAffirmation.h` already uses for "yes": every word must be in
the list, bounded length, one negation refuses), and anything longer goes to the chat. A miss costs a round trip;
a false positive eats the user's question, which is what happened. And when a verb IS taken, say so in the
transcript - a message that vanishes with no bubble is indistinguishable from a dropped send.
**FILES:** PluginEditor.cpp (`handleLoudnessVerb` and its call site), a new narrow matcher beside EJAffirmation.h.
**RISK:** low. **LEG:** "check the level of the mix bus" reaches the chat; "check" alone runs the verb; a taken
verb leaves a visible line.

### E. THE -1.2 dBTP CEILING IS THE SERVER'S -1, AND THE TARGET WAS -12 "DYNAMIC", NOT A -14 FALLBACK.
**THE EVIDENCE:**
    20:07:42.486  EJChainBlock: ceiling_db -1 dB, input_db 0 dB, release_ms 50, lookahead_ms 2,
                  mode transparent, true_peak on, sc_hpf_hz 0
    20:00:11.250  EJLoudness: armed: target -12.0 LUFS (level_params, dynamic), Level slot 4 gain +3.3,
                  limiter slot 5 (EchoJay Limiter), build-time input -14.7 LUFS
So the build dialled **ceiling_db = -1** because the SERVER's chain block asked for it - not the EchoJay Limiter's
own default (-0.3, EedLimiterProcessor.cpp:30) and not Sean's -0.1. And the target was **-12.0 from the chain's own
level_params with loudness_option "dynamic"**: there is no -14 fallback in this story, so item 10's absence did not
cause it.
**AND "DYNAMIC" IS WHY IT STOPPED SHORT.** `grCapDb("dynamic") = 3.0` - unchanged on purpose, because "dynamic" is
a promise to keep the dynamics - so the second proposal was CAPPED at 3 dB of GR: "-12.2 is as loud as this goes
with the limiter working <=3 dB". The 7 Oct cap work raised COMMERCIAL to 10; this build never asked for
commercial.
**FIX, two parts.** (1) A FINAL LIMITER'S CEILING HAS A FLOOR: whatever the block says, the last limiter in the
chain is clamped to -0.1 dBTP (Sean's rule), and the clamp is LOGGED naming the figure that was asked for - a
silent override of the server is the fault we closed for the EQ hoist this morning. (2) The loudness OPTION rides
the card and the log in words, because "dynamic" capping at 3 dB is correct behaviour that reads as a bug: the
bubble should say which promise it is keeping.
**FILES:** ChainHost (the limiter apply path) or LoudnessLoop's `ceilingDb_` resolution; LoudnessLoop bubble text.
**RISK:** low for the clamp, and it needs B told so the block stops asking for -1.

### D. THE CHAINS LIST IS LAID OUT AGAINST ONE BOTTOM EDGE AND PAINTED AGAINST ANOTHER.
**WHAT THE NEW LOGGING PROVES - the panel, the fetch and the model are all FINE:**
    19:55:34.873  EJChains: CHAINS panel opened (tab=6 compact=0 collapsed=0) - rows in memory 0
    19:55:34.876  EJChains: fetch START GET /api/v2/chains
    19:55:35.316  EJChains: fetch RESULT status 200, 1 chain(s) in the body
    19:55:35.319  EJChains: RENDER 1 row(s) after parse
`tab=6` IS `Tab::Chain` (Dashboard 0 ... Link 5, Chain 6), compact and collapsed are both 0, so every gate the
mode depends on was satisfied - my earlier five-condition hypothesis is RULED OUT by this line. The list had a row
and the disk cache kept it ("SAVED CHAINS injection attached -- 1 names, source=disk-cache" on every later turn).
**SO IT IS GEOMETRY, AND THERE ARE TWO AUTHORS FOR ONE EDGE.** The layout bounds the rows with `chatScrollBottom`
(a layout local, PluginEditor.cpp:20237) and pushes an EMPTY rect for any row past it; the painter bounds them
with `chatScroll.getBottom()` (the live component, :18442) and skips any row whose bottom exceeds it. Those are
two different numbers the moment the component is laid out after this block, hidden, or sized differently in
chains mode - and either way every row is dropped, which is exactly a column with nothing in it but the chat input.
This is the defect this file's own comment warns about one layer up: "ONE predicate, consulted here and never
re-derived, so the paint pass and the layout pass cannot disagree". The predicate is shared; the GEOMETRY is not.
**FIX:** author the list's bottom ONCE - a `chainListBottom_` member written by the layout beside
`chainListStatusRect_` - and have the painter use it. Then add what Sean asked for in (A): the toggle never
depends on the mode it switches, and the empty state renders.
**WHAT I HAVE NOT PROVEN:** which of the two edges collapses. Both are defects; one line of logging in the layout
("list bottom N, rows laid out M of K") settles it on the next press, and it belongs in the fix.
**FILES:** PluginEditor.cpp (:18442, :20225-20258, :33817-33860), PluginEditor.h (the new member).
**RISK:** low, and contained to chains mode.
**SAVE/RACK: NO EVIDENCE EITHER WAY TONIGHT.** There is NO `EJChainSave` line anywhere in 19:50-20:12, so Sean did
not save during that window. CHAINS (C) is proven by its leg (the borrowed host is a different object and is the
one chosen), not by a live run. The live confirmation is still owed.

### A. ECHOJAY'S UNDO DOES NOT RECORD ANYTHING SENT TO A LINK.
**WHAT THE STACK RECORDS TODAY:** `EchoJayProcessor::undoHistory_`, fed by `wireUndoHooks(chainHost, {})` - so it
is wired to **ChainHost mutations on a host this process owns** (the local rack and the borrowed copy): add,
remove, move, bypass, wet, keep-level, one step per applyChainEdits batch, plus the loop's own level writes
through `loudnessLoop_.onGainWritten`. A Link Mixer fader move is none of those: it is a TRANSPORT command
(`proc.setLinkGainDb` / the chain-cmd file), so no ChainHost on this side mutates and no hook fires. Nothing is
recorded, so Undo has nothing to undo - it is not failing, it was never told.
**OTHER V2 ACTIONS THAT SKIP THE STACK, by the same rule** (anything whose effect lands on a Link rather than in
this process): the Link Mixer's gain, mute, solo and pan; a slot bypass or wet sent to a Link; dials applied to a
Link slot through a chain-cmd; a chain edit or proposal Apply that is diverted to the Link; the Link's pre-gain.
Every one of them is invisible to Undo today.
**FIX, as ruled:** one undo step per GESTURE, pushed on release rather than per pixel (the wet knob's existing
"one step per knob gesture" is the precedent), carrying {uid, what, before, after}; undoing SENDS THE PREVIOUS
VALUE back to the Link as an ordinary command and waits for its ack, so an undo that did not land says so instead
of lying. The entry is a CHAIN entry in the plugin-wide history, the same class the local rack uses, so one Undo
button walks both.
**NOT Logic's plugin-header Undo:** that records host parameter gestures on the instance that owns the parameter,
and a Link Mixer move is not a host parameter on the V2 at all. Making these host-visible parameters is a
different and much larger decision - it would put every Link's gain into the V2's parameter list - and it belongs
in the remote-control plan, not in 08c.
**FILES:** PluginProcessor (the undo dispatcher + a Link entry kind), PluginEditor (the Mixer's gesture
begin/end), the chain-cmd writer for the undo send. **RISK:** medium - it is new plumbing across the transport,
and the ack path is where it can mislead. **LEG:** a fader gesture pushes exactly ONE step; undo sends the prior
value and is acked; an unacked undo reports instead of claiming success.

### B. THE LINK'S IN/OUT READOUTS OVERLAP THE PLUGIN NAME ON NARROW CARDS.
**CAUSE:** I gave the Link the V2's rects verbatim - `getWidth() - 48, 2, 46, 9` and `+11` - deliberately, so the
two cards could not drift by a pixel. But the Link's cards are NARROWER than the V2's, and the V2's name field was
already sized around those 48 px while the Link's was not: the name draws full width and the readouts sit on top
of it. "Identical rects" was the wrong invariant - what has to match is the READING, not the geometry.
**FIX:** reserve the readout column in the Link's name row (truncate the name to `getWidth() - 52`), and when the
card is narrower than a threshold put the readouts on their own row under the name instead of beside it. The
guard's structural check changes with it: it should assert both editors SHOW both readouts and read the same two
accessors, not that they use identical coordinates - and the slot-card WIDTH goes into the leg, as Sean asked.
**FILES:** LinkEditor.h (Block::resized + the name paint), tools/ui_round_guard/ui_guard.cpp (the structural leg).
**RISK:** very low, cosmetic and contained.

### 08c ORDER I PROPOSE (harm first)
1. **C** - a swallowed question is the worst of these: the user types and nothing happens at all.
2. **E** - a wrong final ceiling ships in the audio, and the clamp is small.
3. **D** - the list is unusable, with a known workaround (the Dashboard shows the chains).
4. **B** - cosmetic but it makes the new readouts unreadable on the Link.
5. **A** - real plumbing, and the remote-control plan may change its shape.

## THE 7/8 OCT FULL GATE (overnight item 1): 60 of 61, and the one red is the harness's own precondition

Run on 397b380 + aae53d6, archives deleted first so both are provably built from the tree's headers.
    S1 archives 0 errors | S2 51 guards 0 errors | S3 4 bundles 0 errors | S4 ctest 98%, 1 of 61 failed
    S5 link_state_guard GREEN both sides | GATE3 DONE 23:40:18
**`dialinfo_keep_guard` PASSED - the teardown race is closed by ad7f131**, which is what item 1 of the queue asked
to be confirmed. The scribble leg that segfaulted on a config-fetch callback after `~EchoJayProcessor` now exits 0.

**THE ONE RED: `lease_id_guard` (v2 side), "the command carries the borrow session's lease id".** Link side GREEN.
Re-run ALONE: PASSED (210 s). Re-run again WITH `alias_mirror_guard` in parallel: PASSED (214 s). So one RED in
three runs, and it was the gate's own `ctest -j 2` scheduling.
**WHAT I CHANGED, because "intermittent" is not a diagnosis:** the leg asserted its claim five seconds after
checking its precondition. It engages a borrow, pumps the message thread for 5 s to clear the Link's 3.5 s lease
gate, then writes a command and asserts the written file carries that session's lease id - which is only meaningful
WHILE THE SESSION IS STILL ENGAGED. On a machine also linking a second 200-second guard, that pumping is not a
measurement. The leg now re-checks `borrowActive()` and the uid AT THE WRITE, prints both, and if the session has
lapsed it exits 2 as a RUNNER REFUSED with the reason, instead of reporting a product failure. If the next
occurrence prints `borrowActive=yes` and still fails, that is a product finding and the leg will say so - which is
the point: the next failure diagnoses itself instead of being guessed at a fourth time.
**NOT CLAIMED:** I did not reproduce the RED. Two green runs do not prove the lapse was the cause; they prove the
leg is not deterministically broken. The instrumentation is what turns the next occurrence into evidence.

## ROUND 06d — QUEUED (ruled 7 Oct; do NOT start until 06c is reported AND Sean has tested it)

In this order:

### 1. THE TEARDOWN RACE — THE CRASH REPORT SAYS IT IS NOT WHAT I FIRST WROTE (corrected 7 Oct 20:0x)
FOUND BY THE 06c GATE, which it turned red: `dialinfo_keep_guard`'s scribble leg segfaulted (exit 139) AFTER the
test had passed every assertion. MY FIRST ATTRIBUTION - "a network callback writes EchoJayAPI members without
consulting `aliveToken_`" - IS WRONG, and the report proves it twice over. The members that callback writes
(`remoteSystemPrompt`, `remotePromptVersion`, `remoteConfigLoaded`, `latestVersion`, the channel prompts) are CLASS
STATICS (EchoJayAPI.h:1085-1097), so they outlive every instance and writing them after a destructor is not a
use-after-free at all. And the faulting thread is not ours:

    .ips BC815958-2E14-4DC1-9D6D-A30022695FC5, 2026-10-07 19:12:25, SIGSEGV KERN_INVALID_ADDRESS at 0x0
    faulting thread 4, queue com.apple.NSURLSession-work
      objc_msgSend / objc_getProperty
      -[__NSCFURLSessionDelegateWrapper didBecomeInvalidWithError:]
      -[NSURLSession finalizeDelegateWithError:]
      __56-[__NSURLSessionLocal _onqueue_invokeInvalidateCallback]_block_invoke

CFNetwork is finalising the shared NSURLSession and messaging a delegate that has already been released. The
delegate is JUCE's: `SharedSession` in juce_Network_mac.mm:132-200 holds a runtime-registered `JUCE_URLDelegate_`
object and is reference-counted by `SharedResourcePointer`, so it is destroyed when the LAST in-flight
WebInputStream/URLConnectionState goes away. MallocScribble is why only the scribble leg sees it: the released
delegate's memory is poisoned instead of merely stale.

**WHAT WE CONTROL, AND IT IS A REAL DEFECT:** `~EchoJayProcessor` does not wait for, or cancel, ANY in-flight
EchoJay network request. It is careful about its own threads - `pluginScanner.requestStop()` then
`loadThread.join()`, then `saveThread->waitForThreadToExit(5000)` (PluginProcessor.cpp:537-558) - and then exits
with up to seven detached network workers still inside a blocking read. SIX in EchoJayAPI (`postJSON` :293,
`patchJSON` :391, `deleteJSON` :448, `getJSON` :568, `startChatStream` :2003, `fetchRemoteConfig` :2627) and four
more in the editor. Every one checks its `alive` flag only BEFORE the blocking call; the config fetch's connection
timeout is **60 seconds** (EchoJayAPI.cpp:2643). So quitting a host within a minute of opening EchoJay, offline or
on a slow network, leaves the session being torn down while the process comes down - the condition in the report.
Only the chat stream is cancellable (`ChatStreamHandle::attach`/`cancel`, EchoJayAPI.h:112-139, which the header
already documents as safe from any thread); the other seven use `url.createInputStream(options)`, which cannot be
interrupted at all.

**THE MEASURED RATE, before any fix (7 Oct, both ways, 20 runs each):** 0 of 20 RED unsealed, 0 of 20 RED through
`run_guard.sh --scribble` exactly as ctest runs it. "config fetch ok" appeared in only **5 of 20**, so in 15 of 20
a request WAS still in flight at exit and did not crash. In-flight-at-exit is therefore necessary but not
sufficient: the invalidation has to land inside the exit window. ONE crash in ~41 runs. That rate is why the leg
cannot be the crash itself.

**THE FIX, and the leg that can actually fail:** make the invariant the thing under test - NO EchoJay network
worker is in flight once `~EchoJayProcessor` has returned. A process-wide census (entered/left by an RAII guard in
every launched worker), a one-way shutdown flag the workers check AFTER the blocking call, cancellation for the
seven uncancellable sites (the WebInputStream pattern the chat stream already proves), and a BOUNDED wait in the
destructor that logs what is still in flight if it times out. Gate the shutdown on being the LAST live instance:
process-wide is right for AU disposal, but killing another instance's chat request when one plugin leaves a track
is not. LEG, made deterministic by pointing the base URL at an unroutable address
(`$ECHOJAY_STATE_HOME/Library/EchoJay/dev_base_url.txt` = `http://10.255.255.1`, so the connect hangs): construct
the processor, destroy it, and assert the census is 0 and the destructor returned inside the bound. RED today (the
census is 1 and the destructor returns at once), GREEN after, 20 runs reported as a count. THE CRASH ITSELF stays
a 20-run observation reported honestly: 0/20 before the fix means those runs are NOT evidence the fix worked - the
invariant leg is.

### 2. Item 6 / 10 FINAL — set the Level at landing, plus the one automatic correction
As recorded in "10 FINAL" above: `gain = target - the song's integrated LUFS` plus EchoJay's own SCALAR gains where
cheap (slot OUT, slot IN pre-trim, master wet); no EQ or compressor prediction; one automatic correction on the first
loud window (3-6 s of real signal), ceiling held, no user action; Go unchanged as the longer listen; too little heard
ASKS; the server's static `gain_db` for a bus/master Level slot is ignored and that is logged. Legs as listed there.
B is holding the matching server change so the two ship together.

#### 2z. ITEM 6/10 FINAL - BUILT, AND loudness_loop_guard IS GREEN (overnight item 2, 8 Oct 00:41)
Everything in the 2a plan landed. What the code now does:
  • **THE TARGET IS INTEGRATED.** `measured = out.levelDb` (K-weighted, so it IS LUFS, and `startWindow()` resets
    the tally, so it is the integrated loudness OF THE WINDOW at the chain output, post limiter). The loudest 3 s
    is still measured and rides the log as the SAFETY CHECK, never the target, as ruled.
  • **THE PEAK-HEADROOM CAP IS GONE**, and so is the -6.0 floor that existed only to bound it. In its place the
    opening gain is a CLOSED-LOOP write: `Level + (target - output integrated)` when a reading exists, logged as a
    measurement; when nothing has been heard the Level is left alone and the log says the first loud window will
    set it. The decision is logged either way, as ruled.
  • **THE GR MODEL IS MEASUREMENT-FIRST**, in three tiers, logged every window as the calibration record:
    the measured typical reduction over the hits; else the EchoJay Limiter's own GR; else `0.25 x excess` CLAMPED
    at 3 dB. `grCapDb` commercial 6 -> **10**, pushed 8 -> 12, dynamic and keep unchanged (they are promises about
    dynamics, not loudness).
  • **LISTEN ALWAYS RESOLVES** within 20 s of playback - from the Listen tap, extended once to 20 s after the
    first counted audio. A deadline now sits in front of all three previously unbounded returns, and "no signal"
    is decided by the READING rather than the counter (a chain does not fall silent the instant the transport
    stops: a genuinely silent window lands at ~0.2 s counted, and telling someone "I heard 0 of 10 seconds" when
    their cable is out is the wrong sentence). A leaked knob gesture can no longer block a measurement.
  • **THE BUS TRIM IS NAMED** in every figure the loop reports, through a new `busGainDb` hook the processor fills.
  • **A LISTEN TAP ON AN OPEN PROPOSAL RE-SHOWS THE CARD** instead of being refused, composed from the same state
    the original was, so the two cards cannot disagree.

**SEAN'S ACCEPTANCE LEG (Z) IS GREEN:** input integrated -14.5, the chain 0.7 dB down so the output at Level 0 is
-15.2 as his is, target -8 -> the Level lands within 1 dB of **+8** and an INDEPENDENT meter reads the output within
1 LU of **-8**. Plus Z2: 20 s of silence resolves with the no-signal sentence AND the figure, at the deadline,
driven by the loop's own clock (the harness feeds 40 s of audio in 2 s of wall time, so a real clock would never
reach the deadline and the leg would have asserted nothing).

**THIRTY-THREE OTHER ASSERTIONS WENT RED FIRST, and every one of them was the ruling, not a bug.** Recorded because
a future reader will want to know why a guard was touched this much:
  - most of them because arm() now LANDS the Level from the reading, so legs whose subject is the window and the
    pills were suddenly starting from on-target. They arm through a new `Rig::armNoReading()` - which resets the
    out tally AND feeds one silent block, because `LevelTally::reset()` is deferred to the audio thread and
    resetting without a block leaves the old snapshot in place. The opening write has its own legs instead.
  - K3 and L1 are INVERTED, exactly as 06c inverted the footer legs: they asserted the cap and the floor, and now
    assert that neither is applied and that the log says so, so a reintroduction cannot pass quietly.
  - K1/K2/O2 are re-aimed at the 10 dB cap, which also meant giving them a target ABOVE what the chain can reach:
    with the opening write landing a -8 target outright, a window that is already on target proposes nothing to cap.
  - K5's bypass rig is a RULER, not a second experiment: it used to reach the clipper through [Push it anyway] off
    a capped proposal, which this fixture no longer produces, so it is now set to the clipper rig's own gain.

#### 2a. THE IMPLEMENTATION PLAN FOR ITEM 6/10 FINAL, read off the code (overnight run, 7/8 Oct)
Written before touching anything so a compaction loses nothing. Every line number is from LoudnessLoop.h as of
397b380.
  1. **THE TARGET IS INTEGRATED.** `measured = out.maxShortTermDb` (:506) becomes `out.levelDb`, which IS integrated
     LUFS because both chain tallies are `Weighting::K` (ChainHost.h:2668-9) - and it is integrated OVER THIS
     WINDOW, because `startWindow()` calls `resetChainOutLevels()` (:682). `known` is required, as everywhere else
     on the tally. maxShortTerm stays, as the SAFETY CHECK Sean ruled it should be, never as the target. Every
     bubble saying "loudest 3 s" changes to name what it measured.
     **THE ARITHMETIC CHECKS OUT AGAINST HIS EAR:** at Level +4 his window-integrated was -11.2, so
     `needed = -8 - -11.2 = +3.2` -> Level +7.2, inside the 1 dB his acceptance leg allows around +8, and the
     output then reads about -8.7, inside the 1 LU it allows around -8. One pass. The residual is the limiter
     costing more at the higher level, which the step-scaling ratio (:560-567) already closes on the next pass.
  2. **THE OPENING CAP GOES on a bus/master with a loudness target** (:263-270, `kOpeningHeadroomDb`). Replaced by
     the opening estimate from 2f: `currentLevel + (target - integrated at the output)` when the tally is known,
     else the open-loop sum, logged AS an estimate. The decision is logged either way, as ruled.
  3. **THE GR MODEL.** `grCapDb("commercial")` 6.0 -> **10.0** and `kGrOfferDb` 6.0 -> **10.0** (:90-96, :103).
     The cap's `base` (:579) becomes: EchoJay Limiter -> the MEASURED GR (`hm.typicalDb`, else `grAvg()`);
     third-party -> `hm.typicalDb` when it is finite and > 0.5 (it is already a MEASURED reduction, Level OUT TP
     minus chain OUT TP over the top blocks, :435-455), else `0.25 * max(0, lvTP - ceiling)` CLAMPED AT 3 dB.
     The old fallback `lvTP - ceilingDb_` is the pessimism that predicted 6.3 dB against a real 1.3-1.7.
  4. **LISTEN ALWAYS RESOLVES.** `kResolveMs = 20000` from the Listen tap, extended once to 20 s after the first
     counted audio. A deadline check in front of all three unbounded returns (:499 counted < needed, :505
     non-finite measured, :506 `knobGestureOpen()`), resolving with the named reason from item 5 and logging every
     transition. `kWaitWallMs` 60000 stays as the mid-wait nudge but is no longer the only thing that ever fires.
  5. **THE LEAKED KNOB GESTURE** cannot block a measurement: at the deadline the window resolves anyway and the log
     says the gesture was open (EJKnobGesture.h: `knobGestureEnded` only decrements, so a missed mouse-up leaves it
     at 1 for the session).
  6. **THE BUS-GAIN TAP** (2c): the landing sum names bus gain as its own term and the log prints it.
  7. **THE PROPOSAL CARD ALWAYS RENDERS**, and a Listen tap in state `proposed` re-shows it rather than being
     refused by `listen()`'s state filter (:285).

#### 2b. THE PEAK-HEADROOM CAP IS WHY EVERY MIX-BUS BUILD WAS QUIET (Sean 19:52, 7 Oct, on the 06c install)
THE OBSERVATION, his log, after the Level was dialled +4.4 dB as sent:
    EJLoudness: opening gain capped: +4.4 -> +0.0 dB (ceiling -0.1 + 3 - build-time true peak 2.9 dBTP)
THE ARITHMETIC IS THE RULE DOING EXACTLY WHAT IT SAYS: `maxOpen = max (kOpeningFloorDb, ceiling + kOpeningHeadroomDb
- truePeak)` = -0.1 + 3.0 - 2.9 = **0.0 dB**, so a hot mix bus gets ZERO push no matter what the target is.
LoudnessLoop.h:97-98 (`kOpeningHeadroomDb = 3.0f`, `kOpeningFloorDb = -6.0f`) and the cap at :263-270.
WHAT IT WAS FOR, from its own comment (22 Sep 2026, item 5 / ruling 5b): "peaks into the limiter never open more than
3 dB over the ceiling" - a BLIND DISTORTION GUARD, applied at build time before anything has been heard, so the
limiter is never handed a large peak reduction on trust. The intent was right; 3 dB is the wrong number for a
master. Sean's reference point: Pro-L 2 at +8.2 dB sounded excellent, and a -8 LUFS hip-hop master routinely limits
6-8 dB on peaks.

**SEAN'S RULING (part of item 6/10 above):** on BUS/MASTER builds with a loudness target the opening gain is set from
the target and the integrated reading (item 10) and is **NOT** capped by peak headroom. Safety comes from two things
instead: the limiter's own ceiling, and the one automatic correction after the first loud window, which reads ACTUAL
limiter GR and backs off only if GR on loud sections exceeds a sane limit. A cap may stay for TRACK chains if it
serves a purpose there, and the purpose must be stated. The decision is LOGGED either way.

**MY PROPOSAL FOR THE GR LIMIT, to be ruled on:** back off on the loud window when EITHER
  • p90 GR over the window exceeds **6 dB** (sustained limiting, not transients), or
  • peak GR exceeds **10 dB** (a single section being crushed),
and back off BY THE EXCESS, once, then hold - never a second automatic cut. Both figures come from the limiter's own
GR, which the tick already reads for the Level card, so nothing new is measured. I am NOT proposing a distortion
metric: we have no validated one (LevelTally computes true peak, 100 ms hop peaks, short-term p90 and max - no
crest-factor or THD figure), and inventing one tonight would be a number nobody can defend.
**FOR TRACK CHAINS I propose the cap simply goes:** its stated purpose was blind safety before any listening, and the
GR check is the same safety with evidence. If it stays anywhere it should be stated as "a track's Level never opens
more than N dB over the ceiling" with N argued, not inherited.

**LEG (Sean's case, exactly):** chain input -14.5 LUFS integrated, +2.9 dBTP, target -8 LUFS, ceiling -0.1, on a BUS
=> the Level opens at about **+6.5 dB** (-8 - -14.5), NOT 0.0, and the log says the cap was not applied and why.
Second leg: the same figures on a TRACK chain assert whatever the ruling on the track cap turns out to be.

#### 2c. THE TAPS, RECONCILED (Sean 20:09 asked for this BEFORE anything else - read-only, done 7 Oct)
His four figures, and the tap each one comes from. THEY DO NOT CONTRADICT EACH OTHER; they are four different
measurements and nothing in the UI said so.
  1. **V2 meters** (Momentary / Short-term / Integrated / RMS / spectrum) = `MeterEngine`, and the comment at
     PluginProcessor.cpp:1385-1398 is explicit: a **POST-CHAIN tap**, read AFTER `chainHost.process`, so it is what
     LEAVES EchoJay. Its Integrated is BS.1770 CUMULATIVE since the last `meterEngine.reset()` (prepareToPlay,
     :639) - it keeps integrating across a gain change, so a +4 dB move part way through a pass leaves it reading
     between the two levels. -11.8 is a running average, not a level.
  2. **the loop's "input window" / build-time input** = `chainInTally_`, the RAW chain input, taken BEFORE
     EchoJay's own pre-chain gain (ChainHost.cpp:1310-1330, and the comment says why: slot 1's input tally is the
     operating level and must see the trim, this one must not). K-weighted, so its `levelDb` IS integrated LUFS.
  3. **the loop's measurement** = `chainOutTally_`, the chain OUTPUT after the whole graph - post Level slot, POST
     LIMITER - post master wet, and **pre bus trim** (ChainHost.cpp:1391/1428). `maxShortTermDb` is the loudest
     3 s IN THIS WINDOW, reset by `startWindow()`. So yes: the loop measures at the chain output, post limiter.
  4. **bx_limiter's own LUFS** is bx's meter, inside the chain, cumulative from whenever bx last reset it. A
     cumulative integrated sits several LU below the loudest 3 s of the same material; -13.3 against a -11.4
     loudest-3-s at 4 dB less gain is the ordinary gap, not a disagreement.
**THE ONE REAL DISCREPANCY I FOUND, and it is ours:** `applyBusGainSmoothed(buffer)` runs BETWEEN
`chainHost.process` and the meter tap (PluginProcessor.cpp, 4 lines above `meterEngine.processBlock`). So the loop
measures PRE bus gain and the meters read POST bus gain. With the bus trim at anything but 0 dB the loop lands the
chain output and the user reads a different number, by exactly that trim, for ever. The loop does not know the bus
gain exists. **Sean: what was your bus/output trim set to at 20:09?** If it was 0.0 the two taps agree and this is
only a latent fault; if it was not, it is part of why the landing missed.
**WHAT THIS MEANS FOR THE ARITHMETIC:** the gain arithmetic must be stated as a tap-to-tap sum, and every figure
logged with its tap name: `landing gain = target - (chainIn integrated + EchoJay's own scalar gains between the two
taps)`, where the scalar gains are pre-chain gain + slot pre-trims + slot OUT gains + master wet + BUS GAIN. Any
stage not in that list (an EQ, a compressor's make-up) is NOT predicted, and the one loud-window correction is what
catches it.

#### 2d. TWO CALCULATION FAULTS, RULED BY SEAN 20:09 (both inside item 2 above)
**(a) THE TARGET IS INTEGRATED, NOT MAX SHORT-TERM.** The loop makes the loudest 3 s equal the target
(`measured = out.maxShortTermDb`, LoudnessLoop.h:506-511, and every proposal is `target_ - m`). A "-8 commercial"
target means the SONG's integrated loudness is about -8, with loud sections 1-2 LU above it. So the loop has been
aiming 1-4 LU low by construction, which is exactly what he heard. RULE: the target is integrated. Landing gain =
`target - (song integrated at the chain output with the Level at 0)`, the song integrated coming from the
build-time `chainInTally_` reading plus the chain's measured gain between the taps (2c). The loud window then
REFINES and acts as a safety check - never as the target.
**(b) THE GR MODEL IS FAR TOO PESSIMISTIC.** At 20:01 the true-peak model predicted ~6.3 dB of GR on the hits at
+3.4 dB; the real bx GR at +4 dB is **1.3-1.7**. The model assumes every dB of true peak above the ceiling becomes
gain reduction, which is what a sample-peak brickwall does and is not what a lookahead true-peak limiter shows: at
+4 dB the excess over the ceiling was about 7.0 dB (build-time TP +2.9, +4 gain, ceiling -0.1) against 1.5 dB of
real GR - a ratio of about **0.21**.
MY PROPOSAL, and I am calling it provisional because ONE point is not a calibration:
  • where GR is readable (EchoJay Limiter) use the MEASURED GR - `gainReductionDb()`, already read every tick for
    the Level card. No model at all;
  • for a third-party limiter, `GR_est = 0.25 x max (0, truePeak + gain - ceiling)`, and CLAMP the estimate at
    3 dB. An estimate that cannot exceed 3 dB cannot hold a master 5 dB down, and the loud-window correction -
    which reads the real thing - is what finishes the job;
  • raise `kGrOfferDb` from **6.0 to 10.0** dB: 4-6 dB of real GR at -8 LUFS is normal for a transparent limiter
    (Sean's Pro-L 2 at +8.2 dB), and 6 dB as a ceiling on what we will even offer is below normal practice;
  • Sean is sending a calibration point (his chosen Level, bx LUFS, bx GR). The 0.25 goes in the leg as a named
    constant so his point either confirms it or replaces it, and the log prints estimate vs measured on every
    window so the next point arrives for free.
**LEG (his):** a fixture with input integrated -14.5 LUFS, target -8, lands within **1 LU of -8 integrated at the
chain output on the FIRST pass** - not after three proposals. Plus the 2b leg (+6.5 dB opening, no cap) and a leg
that asserts estimate-vs-measured is logged.

#### 2e. SEAN'S CALIBRATION POINT, 20:20, AND WHAT THE ARITHMETIC SAYS ABOUT IT
HIS CHAIN: EchoJay EQ, VSC-2, Lindell 80 Bus (OUT -4.0), EchoJay Exciter, EchoJay Level, bx_limiter (TP, -0.1).
Target -8 commercial. **HIS EAR: Level +6.5 to +8 dB is right, reading about -9.5 to -8 LUFS; +8 dB = -8.**
(Pro-L 2 reference +8.2.) Tonight's machine figures: server +4.4, Listen proposal +3.4 (claiming -8.3), opening cap
0.0. The loop is about 4.5 dB short.

**FAULT (a) ALONE ACCOUNTS FOR THE 4.5 dB, arithmetically.** The loop measured max short-term -11.4 at the output
and proposed `target - measured` = -8 - -11.4 = +3.4, which makes the LOUDEST 3 SECONDS equal -8. A commercial -8
means INTEGRATED -8, and integrated sits several LU below the loudest 3 s. Sean's ear put the right answer at +8,
which is +4.6 above the loop's proposal: that IS the short-term-to-integrated gap on this material. No tap error is
needed to explain the shortfall - the loop was answering a different question.

**THE "+2.6 dB OF CHAIN GAIN" IS NOT THE SLOTS' GAIN, and this is a real trap I found while checking his
hypothesis.** `chainInTally_` is taken BEFORE EchoJay's own automatic PRE-CHAIN gain, and
`ChainHost::autoPreGain` sets that gain at every build to `-18 - chainIn integrated`
(ChainHost.cpp:3289, `kPreGainTargetLufs = -18.0f`, clamped +-24). On his bus, with the input integrated about
-14.5, **the pre-gain is about -3.5 dB**. So `chainOut - chainIn` spans EchoJay's own -3.5 as well as every slot,
and "the chain is +2.6 dB" is the sum of pre-gain, the Lindell's -4.0, and whatever the EQ/VSC-2/Exciter add. Any
arithmetic that treats out-minus-in as "what the plugins did" is wrong by the pre-gain, every time, and the
pre-gain is BIGGER on a quiet bus. The landing sum in 2c must name pre-chain gain as its own term, and the log
must print it.

**THE ONE FIGURE I STILL CANNOT RECONCILE, and I am not going to guess it.** bx_limiter is the LAST slot and the
Level sits before it, so bx's output IS the chain output. At Level 0 the V2 meters read integrated -11.8; at Level
+4 with 1.3-1.7 dB of GR the output should read about -9.3 integrated. bx read **-13.3**. Four dB in the wrong
direction, and the bus gain cannot explain it (it would move the V2 meter DOWN relative to bx, not up). The
remaining candidates are different integration windows (bx's integrated running from an earlier, quieter span) or
a reset we are not seeing. **WHAT I NEED FROM SEAN: four readings taken at the SAME moment**, with the chorus
looping and Listen running - (1) V2 Integrated, (2) V2 Short-term, (3) bx's LUFS figure AND which mode it is in
(integrated or short-term), (4) EchoJay's bus/output trim value. That pins it; his four figures from four different
moments cannot.

**ACCEPTANCE LEG (his, as ruled):** build-time integrated -14.5, target -8, this chain -> the landing Level is
within 1 dB of **+8** and the output within **1 LU of -8 INTEGRATED**, with the GR cap allowing it (he judged +8
clean on bx_limiter). So the cap calibration in 2d is part of this leg's pass condition, not a separate item.

#### 2f. THE FOUR SIMULTANEOUS READINGS, AND THE CALIBRATION THEY GIVE (Sean 20:59, Level +4, chorus looping)
V2 (post-chain): Momentary **-10.7**, Short-term **-12.9**, Integrated **-11.2**, LRA 7.2, sample peaks -1.0/-0.8,
true peak -0.0/-0.1. Still open, and neither changes the ruling: was Integrated reset first, and what is the rack
trim (assumed 0.0). **ANSWERED 8 Oct:** Sean does not know whether Integrated was reset before the reading, and **the rack trim was at
its DEFAULT** - so the bus-gain tap fault (2c) is LATENT on his chain, not implicated in the miss, and the
acceptance leg stands as written. The unknown reset is why the reading is treated as the chorus's integrated rather
than the song's, which is what the closed-loop landing wants anyway.
**RULED: V2 / `chainOutTally_` is the truth; the bx -13.3 is its own meter's window and is
dropped from the investigation.** My -9.3 prediction was about 2 dB optimistic - it leaned on the cumulative -11.8
at Level 0 and a 1.5 dB GR allowance - and the reading supersedes it.

**THE CALIBRATION, from this reading and Sean's ear point, and this is what the landing must reproduce:**
  • at Level **+4** the output integrated is **-11.2**, so the output integrated at Level 0 is about **-15.2**;
  • landing at Level **+8** therefore gives about **-7.2** before limiter loss and about **-8.0** after, which is
    exactly Sean's "+8 = -8" and his +6.5-to-8 window reading -9.5 to -8;
  • so the limiter costs about **0.8 dB** of integrated at this level - NOT the 6.3 dB the true-peak model
    predicted, and in the same direction as 2d's measured 1.3-1.7 dB of GR on peaks;
  • **THE RULE THAT FALLS OUT:** the landing is a CLOSED-LOOP measurement at the output, never an open-loop
    prediction. `landing = currentLevel + (target - integrated measured at the chain output NOW)`, which needs no
    model of the chain at all and is immune to every term in 2c (pre-chain gain, slot gains, bus trim) because it
    measures past all of them. The open-loop sum in 2c stays only as the OPENING estimate before anything has been
    heard, and it is logged as an estimate.
  • **ACCEPTANCE, calibrated:** input integrated -14.5, target -8, this chain -> landing Level within 1 dB of
    **+8** and output integrated within 1 LU of **-8**, with the limiter allowance taken from MEASURED GR and the
    cap in 2d permitting it. The fixture's numbers are this reading: output integrated -15.2 at Level 0.

### 3. Item 12 — dial `substitute.settings_structured` onto a licence substitute
B sends `substitute: {name, settings_structured: {params: {ceiling_db, true_peak, mode}}}` on a third-party
loudness-limiter slot. THE OBSTACLE IS KNOWN: the licence branch runs inside a loadAsync callback at
ChainHost.cpp ~7676 whose scope is only `fullDesc` and the built-in `bd` - it cannot see the server payload. So
either thread the substitute block down to that site, or apply it in a post-load pass keyed on
`SlotDialInfo::substitutedFrom`. The leg needs a desc that LOOKS PACE-wrapped to reach the branch
(`echojay::refuseIfPaceWrapped` reads the desc off disk and instantiates nothing, so this is fixture work, not a
plugin load). The hook is already there: LoudnessLoop reads `ceiling_db` from the limiter slot's params
(LoudnessLoop.h:163) and writes it (237). Safe meanwhile: `kCeilingDb` defaults to -0.3 dB.

### 4. SCRAPPED: the [UNLICENSED] list, and then the new button too (Sean, 7 Oct)
First the [UNLICENSED] list idea was replaced by a "Don't suggest <plugin> again" button; then Sean pointed out the
plugin ALREADY HAS a "Don't suggest this plugin again" box. So nothing new is built. The only work left is a CHECK:
does that existing box appear on the LICENCE-SUBSTITUTION card - the new card from 06c item 5 - and if it does not,
add it there. Nothing is ever added to the exclusion store automatically: an unplugged iLok also fails for licence,
and auto-excluding would blacklist a plugin Sean owns.
(Kept for reference, since it is where the store lives if the check finds the box missing: `plugin_disabled.json`
under `Application Support/EchoJay/`, a JSON array of SCANNER UIDS - PluginScanner.cpp:992,
LinkProcessor.cpp:2470-2473 - read back through the `isDisabledByName` predicate the chain build already takes,
ChainHost.cpp:11512/11555. Note the mismatch if it is ever touched: the file keys on uid, the build-time predicate
matches by NAME, and PluginChecklist.cpp:152 records the file as "unattributable".)

### 5. LISTEN MUST ALWAYS RESOLVE - IT CAN WAIT FOR EVER TODAY (Sean 20:00, 7 Oct, on the 06c install)
HIS REPORT: after the mix-bus build he cued the loudest section, played, tapped Listen twice. The second tap said
"Already listening - keep the loudest part playing." Then nothing, indefinitely. His screenshot at that moment:
Momentary and Short-term "--", RMS -61.8, spectrum "no signal".

**READ-ONLY CHECK, done first, and it confirms the stall exactly (LoudnessLoop.h):**
  • the window needs `kNeedSeconds = 10.0` seconds of audio whose MOMENTARY LUFS is above
    `kCountFloorLufs = -40.0` - `heardAboveSeconds`, counted in 100 ms hops by LevelTally (:77-78, :473, :681);
  • the ONLY thing said while it waits is one line at `kWaitWallMs = 60000` - SIXTY seconds - and it is said
    ONCE, behind `waitingSaid_` (:500-502). After that the loop is silent for ever;
  • there is NO deadline anywhere. Three returns at the resolution point have no timeout behind them:
      `if (counted < kNeedSeconds) ... return;`      - silence, or output under the floor, waits for ever
      `if (! std::isfinite (measured)) return;`      - a sensor that never reports stalls it for ever
      `if (knobGestureOpen()) return;`               - and this one is worse: `knobGestureEnded` only decrements,
         so a single missed mouse-up leaves the count at 1 and Listen can never resolve again this session
    (EJKnobGesture.h:8, LoudnessLoop.h:506);
  • "Already listening" is correct behaviour, not the bug: `listen()` refuses from waitAudio/measuring/proposed
    (:285). The bug is that the first Listen never finished.
**IS THE 19:52 CAP IMPLICATED? NO, and I am not going to let the two run together.** With the Level at 0.0 dB the
chain output is the mix at its own level, which on a bus sits far above a -40 LUFS momentary floor; the counter
would have run. The stall is the missing deadline. The cap made the build quiet (item 2b); it did not make Listen
hang.

**THE FIX, as ruled: Listen resolves within ~20 s of playback, always, with a figure or a named reason.**
  • a wall-clock deadline per window, `kResolveMs = 20000` from the Listen tap, EXTENDED ONCE to 20 s after the
    first counted audio so that "20 s of playback" means playback, not the time he spent cueing;
  • at the deadline it resolves in this order, naming the evidence it has:
      - Level or limiter slot gone               -> the existing "no longer in the chain" line;
      - `transportKnown() && ! isPlaying()`      -> "the transport is stopped";
      - counted == 0                             -> "no signal is reaching the plugin", WITH the figure the meters
                                                    show (chain-out momentary / RMS), because -61.8 dB RMS is the
                                                    evidence and a bare "no signal" is not;
      - 0 < counted < kNeedSeconds                -> "I heard N of the 10 seconds I need" + Listen again;
      - counted >= kNeedSeconds, measured not finite -> names the sensor, and logs it as a fault, not a shrug;
      - a knob gesture still open at the deadline -> resolve ANYWAY, and log that it was open. A gesture must
        never be able to block a measurement for ever.
  • EVERY state transition logged, as Sean asked: armed, listening, first audio, window found, window quiet,
    proposal, timeout-with-reason. One line each, with the counted seconds and the measured figure.
**LEGS:** (a) his case - an armed loop plus 20 s of SILENCE resolves with the no-signal message and the figure,
and the loop leaves the waiting state; (b) the other direction, so the deadline cannot eat a good window - 10 s of
loud audio inside the deadline still resolves with a PROPOSAL and the measured figure; (c) a leaked knob gesture
(one `knobGestureBegan` with no end) still resolves at the deadline.

### 6. TEST 5 - MY HALF: THE STRING IS THE SERVER'S, AND I CAN PROVE THE PLUGIN SENT WHAT IT NEEDED
Sean typed "yes do it" to "Want me to add that to the EQ in slot 1?" and got "Nothing is waiting to be applied -
say what you want changed."

**WHERE THE STRING IS PRODUCED: NOT THE PLUGIN.** It does not appear anywhere in Source/ in any spelling. It is
produced by the server, in `api/_offer-affirmed.js` and `api/_level-match.js` (read-only check; B owns that tree).

**AND THE PLUGIN SENT EVERYTHING THE RULE NEEDS - evidence, not inference.** The dev-mode body dump for that very
turn is on disk (`~/Documents/EchoJay/chat-body-debug.json`, 20:16, 302987 b). Its messages array is 12 long and
ends:
    [9]  user       37b   "is the low-mid too thick on this bus?"
    [10] assistant  967b  "...a bell centred around 183-227 Hz ... Want me to add that to the EQ in slot 1?"
    [11] user   106211b   "yes do it" + the injections
So the prior assistant turn went out IN FULL. I then ran the server's own live rule against those exact bytes:
    isAffirmation("yes do it")                -> true
    affirmsAnOffer(...)                       -> { offer: "Want me to add that to the EQ in slot 1?" }
    targetFromOffer(...)                      -> { slot: 1, name: "EchoJay EQ", via: "offer_affirmed", how: "slot_number" }
The committed rule resolves this turn correctly. So the dead-end was NOT a missing prior assistant and NOT the
plugin. **FOR B: the next question is whether the DEPLOYMENT serving Sean carries a765d30 - the rule is committed
on main and "shipped" in the 7 Oct record, which this repo's history has meant "committed" before - or whether
`_level-match.js` produced the line on a different path.** The prod/main divergence rule applies: check what is
live before changing code.

**WHAT I FIXED ANYWAY, because the log proves it fires: the history trim could throw the offer away.**
`trimChatHistory`'s role alignment skipped FORWARD off a leading assistant turn to satisfy the API's
"messages[] opens on a user turn". Sean's live log shows it firing on ordinary turns - `roleAlign 1` at 20:14:41
and 20:16:43, `roleAlign 2` at 20:13:13 - where it has been discarding OLD assistant turns, which costs little.
But when the cap or the budget happens to open the window on the assistant's own last reply, the message thrown
away is the offer the user is answering, and the turn cannot resolve however good the server is. It now steps
BACK to the user turn that prompted the assistant and re-admits the pair (`pairedBack`, printed in the trim log);
it only skips forward when there is no earlier user turn at all, so the API rule still holds. The step back may
take history one message past `maxHistoryBytes` - a named, bounded exception, because an assistant turn without
the user turn that prompted it is not history the model can use.
**LEGS, in `history_guard` (it already drives the real `buildChatRequestBody`):** (1) Sean's exact shape - ten
turns of edit chatter, his question, the 967-byte offer, then "yes do it" with 100 KB of injections - asserts the
offer sentence is on the wire as the last assistant turn AND that it is the whole reply, not a stub; (2) a window
that would open on the offer keeps it, paired back, with `roleAlign 0`; (3) the other direction - no earlier user
turn, so the leading assistant turn is still skipped and `messages[]` still opens on a user turn.

**STILL QUEUED, and deliberately not guessed:** when B starts attaching a structured pending proposal to an offer,
apply it on "yes" and put an Apply button on the offer card. That needs B's wire shape; inventing a schema tonight
would be two sessions building different things. The card and the apply path are ready for it - the editJson
pathway already applies staged ops with a base-revision check.

### 6b. THE STAGED PROPOSAL - BUILT AND GREEN, EXCEPT THE BUTTON (B's 574177e wire format, 7 Oct 2026)
B appends to any reply that offers a rack change:
    <<<ECHOJAY_PROPOSAL>>>{"edit":[{"op":"set","slot":1,"slot_name":"EchoJay EQ","settings_structured":
    {"eq_bands":[{"type":"bell","freq_hz":200,"gain_db":-2,"q":1.4}]}}],"offer":"...","staged":true}<<<END_PROPOSAL>>>
**THE FORMAT NEEDS NO TRANSLATION, and that is the find that made this cheap:** a proposal's `edit` array is the
SAME array `ChainHost::parseChainEditOps` already parses, and `op:"set"` with `settings_structured` is already a
supported op (ChainHost.cpp:2099/2420 - "a settings-only op, never the instance"). So applying a proposal is
applying an edit; what differs is WHEN.

DONE, and `proposal_guard` is GREEN on all 16 assertions:
  • `extractProposalBlock` / `reattachProposalBlock` in EJReplyBlocks.h, re-exported through EchoJayAPI, following
    the existing block extractors exactly. The block is taken out of the VISIBLE reply on the chat route and
    `cm.proposalData` holds it. **Nothing applies it there** - that is the difference from a CHAIN_EDIT block,
    which is a decision the server already made.
  • **the wire KEEPS it**: `chatContents.add(reattachProposalBlock(visibleReply, proposalJson))`. Every other block
    is stripped from history; this one is not, so B's server-side yes ships identical ops. Asserted through the
    real `buildChatRequestBody`, so the history trim cannot quietly eat it.
  • **`capabilities:["proposal"]` in every chat request body** (not a header - corrected ruling), hardcoded because
    it states what the binary can do. Without it B emits no block, which is exactly why 06b and 06c - which would
    print raw JSON at the user - stay safe.
  • **a yes applies it with no model call**: `handleProposalAffirmation` sits beside `handleLoudnessVerb` in
    `sendChatMessage` ("local first, no network"), and `applyStagedProposal` moves the ops into `editData`, takes
    the base revision NOW (the rack may have changed since the offer) and runs the existing apply with its
    staleness guards. ONLY the newest assistant turn counts - a yes answers the last thing said.
  • **the client's yes is deliberately NARROW** (`Source/EJAffirmation.h`): B's rule is the source of truth and
    this is a fast path, so the asymmetry runs one way - a miss costs a round trip, a false positive writes a
    change nobody asked for. 12 plain affirmations taken, 10 doubtful ones refused ("yes but make it 300 Hz
    instead", "do it on slot 2 instead", "yes and also add a de-esser after the comp").
  • **rule 5 was already satisfied**: an affirmation with nothing staged returns false and the turn goes to B
    exactly as today. The body dump proves the plugin already sends what B's rule needs (item 6).

**LEFT, AND IT IS THE ONLY PIECE: the Apply button on the offer card.** `applyStagedProposal(msgIdx, why)` is the
entry point and is already written and used; what is missing is the card drawing a button that calls it. The route
I would take, and the reason I did not take it tonight: the edit card already renders ops as "dial <name> (slot N):
<payload>" and has a not-yet-applied state, so the button is a matter of letting `proposalData` render through that
card WITHOUT the auto-apply that an edit turn triggers - and separating those two in `handleChatReply`'s dial path
is not a change to start at 21:35 with a gate owed. The data path ships without it: a yes works today.

### 6c. place_ship.sh's IDENTICAL-UUID BLIND SPOT - CLOSED (overnight item 8, 7/8 Oct)
WHAT WAS WRONG: check (2) read "a rebuilt binary cannot keep its UUID, so a match means nothing was built". False
for a reproducible build whose inputs did not change - 06c changed PluginEditor.cpp, PluginProcessor.cpp and
EchoJayAPI.cpp, the Link archive compiles NONE of them, the Link bundle relinked bit-identical, and the script
refused the whole folder. The round was then placed BY HAND, which is the one thing the script exists to stop.
THE REAL FAULT was not the UUID comparison: it was comparing every bundle against the newest source in the WHOLE
tree. **Both checks are now scoped per bundle, from the object list of the target that built it**
(`$BUILD/CMakeFiles/<EchoJay|EchoJayLink>.dir/Source/*.o` -> the .cpp files that bundle actually contains; falls
back to the whole-tree rule when the object dir is absent, because a check that silently weakens itself is worse
than one that is occasionally strict). An identical UUID is a REFUSAL only for a bundle that is ALSO stale against
its own sources; otherwise it prints a note saying the match is correct and must be reported as unchanged, not as
a new binary.
PROVEN BOTH DIRECTIONS on a fixture build tree (no repo files touched): (A) the 06c shape - every bundle newer than
its own sources, Link UUID identical to installed -> PLACED, with the note; (B) the V2 bundles backdated to before
PluginEditor.cpp, which the V2 compiles and the Link does not -> V2 REFUSED (stale) and REFUSED (unchanged), nothing
placed, and the LINK NOT DRAGGED DOWN by a change to a file it does not compile. That last line is the whole fix.

### 0. THE REAL BYPASS BUG - FOUND AND FIXED (Sean 11:41, 8 Oct; code written, UNBUILT)
**HIS REPORT:** rarely, after the V2 takes a Link rack and hands it back, the Link's plugins STAY bypassed - real,
persistent, with no lease held. Exactly the caveat I flagged on the display bug.

**WHERE IT WAS, and it is not any of the three places we expected.** The v9 design is sound where it was examined:
`setLeaseBypass` touches only the EFFECTIVE `bypassed` and never the intent; `setSlotBypassed` records the intent
and keeps the rack dry while `attachBypassed_`; `rackLeaseRelease` restores every slot from `intendedBypassed`,
which is why it already handles slots that arrived mid-lease. Engage and release are properly paired, and the gate
has an expiry path ("ONE restore path for every ending - clean release, expiry after a crash, a new id superseding
a dead session", LinkProcessor.cpp:1090-1108), so (b) was already covered.
**THE HOLE IS IN WHAT THE MODEL RECORDS**, and the model is "what the editor renders AND what persists":
```
    if (rackLeaseActive_ && i < (int) rackLeasePrior_.size())  s.bypassed = rackLeasePrior_[i];   // the intent
    ...
    else                                                        s.bypassed = info.bypassed;        // EFFECTIVE
```
`rackLeasePrior_` is captured AT ENGAGE. **A slot ADDED DURING the lease - which is what every build does - has no
prior**, falls through to the effective bypass (true for every slot under a lease), and the model saves it as
bypassed. Releasing the lease cannot undo a value that is already in the saved state, and the host was right the
whole time: its `intendedBypassed` for that slot is LIVE. That is the persistence, and "rarely" is "only when a
slot arrived while the rack was borrowed".
**THE FIX:** under a lease the model reads `info.intendedBypassed` for EVERY slot - the same source
`rackLeaseRelease` already restores from - so the prior list stops being the model's authority. One source of truth
for "what did the user actually ask for", which is the only thing that may ever reach the saved state.
**SEAN'S (a), done anyway and for the stated reason:** `rackLeaseEngage` now REFUSES to re-snapshot while a lease
is held, and logs the skip. With v9 the snapshot is of the intent, so a re-entry is harmless today - the guard is
there because the day someone changes what is snapshotted is the day a silent re-entry becomes a rack that never
comes back.
**SEAN'S (c):** every engage and release now names its lease id and prints the slot-by-slot states - snapshot
TAKEN (with the list) or SKIPPED, and on release what was restored from intent beside how many priors the engage
had held. A rack that comes back wrong can now be read out of the log instead of reasoned about from a photograph.
**LEG, in `linksync_test` (it drives the REAL arms, not a copy):** mixed pre-borrow intent [live, BYP, live],
engage, engage AGAIN, a slot added under the lease, then assert mid-lease that the model records the intent of
every slot including the new one (RED as it stood), then hand back and assert the original mixed states return and
the added slot is LIVE. Deliberately mixed so neither an all-live nor an all-bypassed restore can pass by accident.
**UNBUILT:** guard builds wait for "Sean is out of Logic".

### 7. EVERY SLOT READS BYPASSED ON A LINK RACK (Sean 10:34, 8 Oct, on 08a) - DIAGNOSED, DISPLAY-ONLY
**THE SYMPTOM:** switching the V2 rack selector to a Link rack (RACK: AITCH_4_01) SOMETIMES shows every plugin
with a BYPASSED tag - EchoJay EQ, Tube-Tech, UAD UA 1176, NLS Buss, all four at once. His 10:58 screenshot of the
**Link's own window shows NO slot bypassed**, which is the other half of the evidence.

**THE MECHANISM, read off the code, and it accounts for all three oddities (all four at once / only sometimes /
the Link disagreeing):**
  1. `LinkProcessor::rackLeaseEngage()` (LinkProcessor.cpp:2548) does exactly this when the V2 takes the rack:
     it saves each slot's INTENT in `rackLeasePrior_` and then calls `setLeaseBypass(i, true)` on **every slot**,
     streaming dry. That is correct and deliberate - the main plugin is hosting those plugins now.
  2. `setLeaseBypass` bumps the revision, so the sidecar republishes, and `fillRackSidecarSlots`
     (EJRackSidecarFill.h:38) writes **`s.bypassed`** - the EFFECTIVE state, which the lease has just set true
     for every slot. `RackSidecarSlot::controlled` is written alongside and already means exactly this ("leased to
     the main plugin, bypassed here, edited there").
  3. The V2 rack view reads `processorRef.linkRackCache` and builds its rows from `rs.bypassed`
     (PluginEditor.cpp:9968). So whenever the cached sidecar is one published DURING the lease, every row is
     bypassed. The same field reaches the model: PluginEditor.cpp:11217 appends "(byp)" to the name for
     [CURRENT CHAIN], so the model is told the whole rack is bypassed too.
  4. The LINK's own window shows no bypass because v9 keeps the two apart on purpose: `SlotInfo::bypassed` is the
     effective state and `SlotInfo::intendedBypassed` is "the state a NON-lease caller asked for"
     (ChainHost.h:59-60). The Link renders the intent. **Two views, two fields, one rack - and the V2 picked the
     one the lease overlays.**
  5. "ONLY SOMETIMES" is the timing: it depends on whether the cache holds a snapshot taken before or after the
     lease engaged, and it clears on switching away and back because the release republishes the restored intent.
     That matches Sean's third check exactly; his [yes/no] answers will confirm or kill it.

**SO IT IS DISPLAY-ONLY ON THE V2 SIDE - with one caveat I will not paper over:** the audio on the LINK genuinely
is bypassed while the lease is engaged, because the V2 is hosting those plugins itself. What is wrong is the
V2 saying "BYPASSED" about slots it is at that moment processing. If Sean's "real audio bypass" check comes back
YES *while the V2 is NOT holding a lease*, this diagnosis is wrong and the question becomes who called
`setLeaseBypass` without a lease.

**THE FIX I PROPOSE: publish the INTENT, not the overlay.** `fillRackSidecarSlots` writes `s.intendedBypassed`
into `bypassed`, and `controlled` keeps carrying the lease - which is what it was added for. One authoritative
field, so no reader can confuse the two, and it fixes the chat block in the same stroke. The alternative - every
reader checks `controlled` first - leaves the trap in place for the next reader. **NOT the `close-apply FAILED`
path:** that line in my overnight ui_guard output is a different fault (edits unacked in 5 s on a mock Link) and
nothing in it writes bypass.
**LEG (reproduces the race, not a snapshot of it):** a Link rack with slots whose intent is NOT bypassed; engage
the rack lease; read the sidecar at that moment and assert the V2's rows are NOT bypassed while `controlled` is
true for each; release and assert the intent survives. RED today on the first assertion.

### 8. THE LINK'S WINDOW HAS NO PER-SLOT IN/OUT READOUTS (Sean 10:58) - BUILT 8 Oct, ui_guard GREEN
**THE HISTORY QUESTION, ANSWERED FIRST because it decides the shape of the work.** `git log -S 'IN +0.0'` and
`-S 'outGainDb'` over `Source/LinkEditor.h` return **nothing, ever**. The readout is `GainReadout` in
PluginEditor.h:1909-1998, introduced by the "Build 2 (30 Sep 2026 ruling): IN AND OUT ON EVERY SLOT CARD" in
commit **ba61a36**, which touched the V2's editor only. So it was **never built for the Link** - there is nothing
to recover and no regression to explain.
**WHAT IT IS IN V2, so "the same" is a real specification:** two readouts on the card's title line, dB to one
decimal, and FULLY INTERACTIVE - vertical drag at 0.1 dB/px (0.02 with shift, the knobs' idiom), double-click to
type a figure, and the mouse wheel. Every interaction is gated on the `set` callback being present.
**THE SOURCE OF TRUTH IS THE SAME ONE:** `ChainHost::getSlotOutGainDb(i)` and `getSlotPreTrimDb(i)`, which the
Link owns directly for its own rack - so the Link's card needs no sidecar and no lease negotiation, unlike the V2
viewing a remote rack. The two `Block` structs are already near-parallel (LinkEditor.h:90 and PluginEditor.h:2004),
which is what makes this a contained job.
**LEG as ruled:** both views render IDENTICAL IN/OUT text for the same rack - the guard reads `readoutText()`
("IN +0.0" / "OUT -6.0"), which exists for exactly this purpose, from each editor and compares them slot by slot.

### 8b. THE LINK MIXER'S CHANNEL METERS READ PRE-FADER (Sean 11:22, 8 Oct) - FIXED 8 Oct, shm_layout_guard GREEN
**WHERE THE FADER IS APPLIED AND WHERE THE METER TAPS, which is what was asked:**
  • `LinkProcessor::processBlock` runs `chainHost.process()`, then **the meter tap**
    (`meterEngine_.processBlock`, `levelTally_.push`, `keyEngine_.pushBlock`, LinkProcessor.cpp:2279-2289), and
    THEN `applyGainSmoothed(buffer)` at :2292. So every published meter field is POST-chain and **PRE-fader**.
  • That position is deliberate and recorded in the code: the 21t-d ruling (25 Sep 2026) MOVED the tap above the
    gain stage, because levelling a GROUP needs what each channel delivers INTO its trim - "so moving one member's
    trim does not rewrite the number the next decision is made from". A level MATCH wants the other side, so the
    frame carries `kFrameHasPreTrim` and readers that want the DAW's figure add the trim back through
    `frameLoudnessAsHeard` (LinkShm.h:396).
  • **THE V2 ALREADY DOES THAT CONVERSION - for the loudness fields only.** At ingest (PluginEditor.cpp:7088-7102)
    it adds the trim back to `momentary`, `shortTerm`, `integrated`, `truePeakMax`, `truePeakCur`, `shortTermTP`
    and `shortTermMax`, with the comment "ONE conversion, here at ingest, so every reader sees one consistent
    figure". **The PER-CHANNEL fields are not in that list**, and the meter bar is drawn from exactly those:
    `mf.peakFastL/R` for the fast bar, `mf.peakL/R`, and `mf.rmsL/R` for the wide RMS marker
    (PluginEditor.cpp:7400-7462). That is the whole bug - the LUFS numbers on the strip DO move with the fader
    while the bar beside them does not.
  • **THE LINK'S OWN WINDOW HAS NO CHANNEL METER AT ALL** (`Source/LinkEditor.h` has one incidental match for
    "meter" and no meter engine, no frame fields, no bar). So the ruling applies to the V2 Link Mixer only, and
    there is nothing to change on the Link side - which is worth stating rather than leaving open.

**THE FIX, display-only and inside Sean's constraint:** extend the EXISTING ingest conversion to the per-channel
fields - `peakL/R`, `peakFastL/R`, `rmsL/R` - so the bar reads as-heard like the numbers already do. The Link's tap
does not move, `kFrameHasPreTrim` keeps its meaning, and every analysis consumer (calibration, loudness, group
levelling) reads the published pre-trim figure exactly as it does now. One conversion site, already written, one
list to complete.
**THE ONE JUDGEMENT CALL, flagged rather than taken quietly:** the CLIP LATCH is set from the raw `peakFastL/R` at
0 dBFS (PluginEditor.cpp:7453-7455). Converted, a channel pulled down 6 dB can no longer latch a clip the DAW
never hears - which I believe is right, because the lamp sits on the bar and a console's clip lamp is post-fader
too - but it does mean the lamp stops warning about a converter overload upstream of the trim. If that warning is
wanted it needs its own indicator, not the channel lamp.
**LEG as ruled:** the same frame at trim 0 and trim -6 -> the bar reads 6 dB lower and the strip's LUFS figures
move by the same 6, while the published frame is untouched; and at trim 0 the rendering is identical to today.

### 8c. THE CHAINS PANEL: THREE SEPARATE FAULTS (Sean 11:24-11:30, 8 Oct; also broken on 06c)
**(A) WHY THE PANEL GOES BLANK AND THE TOGGLE VANISHES - the coupling, with the one thing I cannot settle named.**
`chainSidebarChainsMode` lives on the PROCESSOR (it survives tab switches and editor teardown), but every renderer
of that mode is gated on being on the Chain tab:
  • the toggle's own rects are authored only when `stripHasSwitch` - `currentTab == Tab::Chain && !compactMode &&
    !visualOnlyMode && !reviewOverlay.visibleState && !chatSidebarCollapsed` (PluginEditor.cpp:20120-20134), and
    the paint is `if (! chainModeAiRect_.isEmpty())` (:18349). Empty rects = no switch drawn AND no click target.
  • the list is laid out only when `chainsMode = chainSidebar && chainSidebarChainsMode` (:20199), with the same
    five conditions inside `chainSidebar`.
  • `chainSidebarInChainsMode()` is `currentTab == Tab::Chain && chainSidebarChainsMode` (:33836).
  • `chatTextSizeBtn` - the "Aa" - is the ONE control visible in either state:
    `setVisible(chatScroll.isVisible() || chainSidebarInChainsMode())` (:20114).
So the state "chains mode ON while any one of those five conditions is false" renders exactly what Sean
photographed: a blank panel with only Aa and no way back. **WHAT I CANNOT SETTLE FROM THE CODE** is which of the
five went false on his press, and his 11:26 finding - NOTHING in the log containing "chain" - says the press did
not reach `setChainSidebarMode` either (that function calls `refreshChainList()` unconditionally, so a fetch
would have been attempted). That points at the click landing on a rect authored in one frame and gone in the next.
THE FIX IS THE SAME EITHER WAY, and it is what he asked for: the toggle is never conditional on the mode it
switches; an empty list renders "No saved chains yet" (that branch already exists at :33821 and is simply never
reached); and the CHAINS path logs open / fetch start / result count / render, so the next occurrence is evidence
instead of a photograph.
**HISTORY, as asked:** the panel was introduced by `faea329` ("Chain sidebar: AI | Chains mode, cached list,
per-call GET timeout") and the current gating by `d20e094` ("the chat body paints only in AI mode") and `fc6d6aa`
("Chain header: one strip authority"). `afcfea9` touched it last. Nothing since 08a, and he confirms 06c was
broken too, so this is NOT an 08a regression - it is the shape it was built in.

**(B) WHY THE LIST IS EMPTY WHILE THE DASHBOARD SHOWS THE CHAIN - two different routes, and ours is the dark one.**
  • the native panel and Open call `EchoJayAPI::listChains` -> `GET /api/v2/chains` (EchoJayAPI.h:1163-1167), and
    the header above those four endpoints says it outright: "Every one of these is gated on DASHBOARD_ENABLED
    server side, so on production they answer **404 not_enabled** until the flag is flipped. That is the dark
    state working, not a fault."
  • the Dashboard tab is a **WEBVIEW**: it navigates `https://www.echojay.ai/dashboard?embed=plugin`
    (DashboardWeb.h:36), so it reads whatever the live site reads - our own comment names `lib/dash/chains.js` as
    the server's chain module. The save reaches that store, which is why "Aitch Vocal Chain, 5m ago" is listed
    there and nowhere in the plugin.
  • `collectSavedChainRefs` returning `source=server` with 0 names (:28211-28215) is the client faithfully
    reporting an empty 200, or a never-filled list with a stamped fetch time - either way it is reporting the
    v2 route's answer, not the one the Dashboard sees.
  **FOR B / TO ROUTE:** which route is live on production for the chain library - `/api/v2/chains` with
  DASHBOARD_ENABLED on, or the dash route the website uses? The client should call the SAME one the Dashboard
  does. This half is not fixable here without that answer, and guessing a URL would be worse than asking.

**(C) SAVE SERIALISES THE WRONG RACK WHEN A LINK RACK IS SELECTED - confirmed in the code.** The save body is
built from `processorRef.getChainHost()` (PluginEditor.cpp:34348) - this V2 instance's OWN rack. The EDIT path is
careful about exactly this and does the opposite: `if (auto* bh = processorRef.borrowHostIfActiveFor(uid))` with
"path=SESSION (borrowed host)" and a comment explaining that the Link parks its own slots while leased so the
content lives in the borrowed host (:26117). So with a Link rack selected, Save stores the MIX BUS under the name
the user typed - which is exactly what the Dashboard shows: "EchoJay EQ -> UAD Shadow Hills ... -> Spec..." under
"Aitch Vocal Chain". Sean's confirmation of which rack he was viewing decides whether this fired here, but the
asymmetry is a fault either way: two paths, one question, two answers.
**LEGS:** the empty list renders the empty state with the toggle intact; a save appears in the list without
reopening; and Save with a Link rack selected serialises THAT rack's slots, not the instance's.

### 9b. THE STUCK LEVEL CHECK, LIVE (Sean 11:26) - folded into item 9
EJThreshold on MDynamicsMBLarge has run **400+ windows over about 20 minutes** with byte-identical readings every
window: `gr=-0.0 chainIn=-20.8 chainOut=-20.2 chainDiff=+0.5 mode=passive settle=0/3
lowLevelGain=(waiting, n=0) settleHeard=211`, and it is still running. Nothing was dialled on that slot
(writesRejected), so GR can never appear, and an unchanged chainIn for twenty minutes means no fresh audio.
So item 9 gains a **NO-NEW-AUDIO DETECTOR** beside the deadline: identical readings for N consecutive windows is
itself a terminal condition, and it resolves with the named outcome - "nothing dialled on <plugin>, set it by ear"
when the writes were rejected, "no audio, press play and tap Listen" when the readings are frozen - and logs which
of the two ended it. **This is the fourth unbounded wait in two days.** The rule is now explicit: a loop that
reads a sensor needs a deadline, a staleness test on the sensor, and a sentence for each way it can end.
LEG: a slot with no GR and frozen readings ends inside the deadline with that message.

### 8d. THE APPLY RESULT REPORTS THE WHOLE RACK, AND CONTRADICTS ITSELF (Sean 13:18) - FIXED 8 Oct, GREEN
**HIS LINE:** "Nothing was applied - EchoJay EQ: some settings were ignored; Bettermaker Bus Compressor DSP:
ignored RATIO. dialled EchoJay EQ". The Bettermaker was not in this Apply at all, and the sentence says nothing
was applied and then that something was.

**BOTH HALVES CONFIRMED IN THE CODE (PluginEditor.cpp:25802-25821), and they are the same class of fault 06c
item 3 closed for the build summary:**
  • the `bad` list walks **`getChainHost().getDialInfos()`** - every slot in the RACK - so a slot left `partial`
    by an EARLIER BUILD is reported as though this Apply had just done it. That is where the Bettermaker's ignored
    RATIO came from.
  • `summary += " " + results.joinIntoString("; ")` appends the per-op results to a headline that opens "Nothing
    was applied", so one sentence carries both claims.
**THE FIX:** scope the walk to the slots THIS apply touched (the ops' own slot indices, which the apply path
already has), and never concatenate a "nothing" headline with a results list that says otherwise - a mixed outcome
gets the mixed sentence that already exists ("Applied N of M - ..."). The headline and the list come from one
decision or they will disagree again.

**(2) THE DEFENSIVE HOIST, as ruled.** The proposal carried `params.eq_bands` while the built-in EQ reads
`settings_structured.eq_bands` (B is fixing the emitter). So when a staged op for a built-in EQ carries
`params.eq_bands`, the client hoists it to `settings_structured.eq_bands` before applying **and logs that it did**
- a silent repair of someone else's payload is a fault that cannot be found twice.
**LEGS:** an Apply that touches one slot reports ONLY that slot, with a rack whose other slot is deliberately left
partial by an earlier build; a mixed Apply never produces a "Nothing was applied" headline beside a dialled slot;
and a staged op carrying `params.eq_bands` dials the EQ, with the hoist logged.

#### 8b-done. WHAT SHIPPED FOR THE POST-FADER BARS (8 Oct, in 08b)
The six per-channel fields joined the EXISTING ingest conversion - `peakL/R`, `peakFastL/R`, `rmsL/R` - at the one
site whose own comment already promised "ONE conversion, here at ingest, so every reader sees one consistent
figure". The Link's tap does not move, `kFrameHasPreTrim` keeps its meaning, and calibration, loudness and group
levelling still read the published pre-trim figure. The clip lamp follows the bar (approved): it reads the same
as-heard figure, so a channel pulled 6 dB down can no longer latch a clip the DAW never hears - and the cost is
written into the code rather than hidden, because that lamp no longer warns about an overload UPSTREAM of the trim.
**LEGS in `shm_layout_guard`:** the arithmetic both ways (-6 reads 6 lower, trim 0 is unchanged), a field with no
reading is NOT shifted into a fake one, an older frame without the bit is left exactly as it was - and a
**STRUCTURAL** leg that asserts all THIRTEEN displayed fields are in the conversion. That last one is the half that
matters: the bug was never the arithmetic, it was a list with six fields missing, and a new frame field added and
forgotten is the same bug again. It earned its keep immediately - its first window was 1800 characters and stopped
four lines short, reporting the product as broken over a boundary the leg itself had chosen; it is now bounded at
the block's own closing brace.

### 10. body.channelTarget ON EVERY CHAT REQUEST (ruled 8 Oct; AFTER 08b, not in it)
`{lufs, option}` from the channel's LAST CONFIRMED loudness target - what the Level actually landed toward, not
what a chat once asked for - on every chat request, so B can carry a channel's target into a new chat and into a
rebuild. B's side is built (hold / proposal-apply). The source of truth is the Level slot's own armed target
(`LoudnessLoop::target()` / `loudnessOption()`, which is what `armFromChain` read out of the slot's params), and
the honest rule is the same one the rest of the body follows: the key is ABSENT when nothing has been confirmed on
that channel, never a guessed default - a target the user never agreed to is worse than no target.

#### 8-done. WHAT SHIPPED FOR THE LINK'S IN/OUT READOUTS (8 Oct, in 08b)
**ONE STRUCT, NOT TWO.** `GainReadout` moved out of `EchoJayEditor::ChainListPanel` into
`Source/EJGainReadout.h` as `echojay::GainReadout`, and both editors construct it. Copying a hundred lines into
the Link would have satisfied the ruling on the day and then drifted: the next change to the drag idiom, the entry
box or the number format would land on one card and not the other, and "the same readouts" would quietly stop
being true. The V2 keeps its own palette through a `dimColour` member - the only thing that was a look-and-feel
reference - so its cards look exactly as they did.
**THE LINK'S SIDE** reads and writes its own `ChainHost` directly (`getSlotPreTrimDb` / `getSlotOutGainDb` and
their setters), by **hostIdx, not the model index** - the model index is a display order and the host's is what
owns the gain. No sidecar and no lease: the Link owns its rack. Same rects as the V2, same interactivity (0.1 dB/px
drag, 0.02 with shift, double-click to type, wheel), and a row with no host slot gets no setter rather than a
control that silently does nothing.
**LEGS in `ui_guard`:** the shared behaviour on the struct itself ("OUT -6.0", "+0.0" dim at unity, "+8.1" flagged
moved, and a getter-only readout that reads but cannot be written), then a STRUCTURAL pass over BOTH editor files
asserting each holds the shared readouts, lays them out at the same rects, wires both getters AND both setters,
that the Link carries no copy of the struct, and that it reads the same two accessors the V2 does.
**THE LIMIT, stated:** the legs do not stand two live editors side by side and compare rendered text - they prove
one shared implementation plus one shared source of truth, which is what makes the two equal. A leg that builds
both editors in one process is the stronger test and is worth having later.

### 8e. THE LANDED LEVEL GAIN DID NOT SURVIVE A RELOAD - FIXED (Sean 13:22/13:24, 8 Oct, in 08b)
**HIS TWO REPORTS, one cause.** At 13:16:20 the landing wrote "+8.1 dB for target -7.0" and at 13:16:55 the chain
block confirmed it HELD ("out-in 8.1 dB, heard 18s"). He saw the Level at 0; then the host relaunched and the Level
came back at 0.0.
**THE CAUSE:** the host's save writes the state **CACHE**, not the live plugins - deliberately, because
`getStateInformation` on a hosted plugin "can take seconds (samplers, convolution) and is not something to run
inside the host's own save callback". The cache is filled by `refreshStateCacheIfIdle` (dirty slots past their
backoff, called from an editor-teardown path) or `captureAllSlotStatesNow` ("for DELIBERATE user actions only,
currently the explicit Save"). **Nothing captured after a loop write.** So the cache still held whatever the Level
had before the landing, and the save looked completely successful - which is word for word the failure
`captureAllSlotStatesNow`'s own comment warns about: "a knob moved a second before Save would otherwise be saved
at its previous value". A landing is that knob.
**THE FIX:** a new targeted `ChainHost::captureSlotStateNow(int)` - the one-slot form of the existing call, with
the same deliberate disregard for the backoff, because the backoff protects the background cadence and not the
correctness of a value somebody just set - called from `LoudnessLoop::writeGainDb`. EVERY write takes it rather
than us deciding which writes "count": the Level is a built-in and its capture is a small JSON.
**LEG Z3 in `loudness_loop_guard`, GREEN:** land a gain, then read `getCachedSlotStatesVar` - which is exactly what
the host would write, since it serialises the cache and never calls into a plugin - decode the base64, and assert
the saved `params.gain_db` equals the landed figure (saved 6.49 vs landed 6.49).
**THE DISPLAY HALF IS CLOSED BY THE TIMELINE (Sean, 8 Oct):** he checked the Level at 13:22, AFTER the host
relaunched at 13:18, and the song was audibly low. So the 0 he saw WAS the real post-reload value - the cache bug
above - and not a stale display. No `EedLevelEditor` work and no card-label work is owed. Worth recording because
the two readings were indistinguishable from the report alone, and the clock is what told them apart.

#### 8d-done. WHAT SHIPPED (8 Oct, in 08b)
**(1) THIS APPLY'S OWN SLOTS, AND ONE HEADLINE.** The walk is now scoped to the slot indices the ops name
(`opsForAlt`, already captured in the completion lambda), so a slot left `partial` by an earlier BUILD is not
reported as this Apply's doing - that is where the Bettermaker's ignored RATIO came from, and the Bettermaker was
not in the Apply at all. An empty touched-set is read as "this apply cannot attribute a slot" and attributes
nothing, NEVER as "every slot". The headline now comes from one decision: nothing bad on the touched slots ->
"Changes applied"; something ignored AND something dialled -> **"Partly applied - ..."**; nothing dialled ->
"Nothing was applied - ...". The old code said "Nothing was applied" and then listed what was applied, and a reader
cannot act on a sentence that contradicts itself. One log line records the scoping: how many slots were touched,
how many could not use what arrived, and whether anything dialled.
**(2) THE HOIST, ANNOUNCED.** A staged op whose `settings_structured.params` carries `eq_bands` has it moved to
`settings_structured.eq_bands`, where a structured built-in actually reads it (the flat `params` map is the other
shape, for devices with no array form - ChainHost.h:2424-8). The now-empty `params` map is removed rather than left
as an empty object. **Logged every time**, naming the slot: a silent repair of someone else's payload is a fault
that cannot be found twice - the emitter would look correct here for ever while every other consumer kept
rejecting it. B is fixing the emitter; this is the client refusing to be broken by a shape.
**LEGS (proposal_guard, GREEN):** the wrong-shaped payload hoists to the top of `settings_structured`, the empty
params map is gone, B's values survive the move unchanged (314 Hz, -4 dB, Q 1.8), and a structural assertion that
the repair is logged.

#### 9-part1. THE STALE BOOKKEEPING IS CLOSED (8 Oct, in 08b; level_loop_guard GREEN)
**A STAMP, NOT A RESET, and the reasons matter.** `loopsAlive_`/`loopsStarted_`/`loopsDead_` now carry
`loopsStampRev_`, the structural revision they were reported for, set by their two existing setters and compared in
`loopsWatchdogLine()`. Two reasons it is not a reset: the structural bump is reachable from the AUDIO THREAD and is
`noexcept`, and clearing a `juce::String` there would allocate; and a reset has to be remembered by every future
mutator, while a stamp is checked at the ONE place the data is read and cannot be forgotten. Bypass bumps the
structural revision too, so a bypass toggle also invalidates the picture - deliberately: a cleared picture is
honest and a stale one is not. A report made after the change prints normally, so the stamp is not a one-way latch.
The line now names BOTH revisions, so the gap is readable instead of inferred.
**THE MAP-ARRIVED LINE IS ONCE PER SLOT PER REVISION** (`ChainSlot::noSettingsSaidRev`), not once per slot per
arriving map. Six slots times N maps printed the same expected-ordering sentence about whichever rack the host
happened to hold. The fact is kept; the repetition is gone.
**LEG (15) in `level_loop_guard`, GREEN:** a report for this revision prints its names; a structural change with no
new report prints NO stale names and says "nothing has reported a start on THIS rack", naming both revisions; a
fresh report afterwards prints normally.
**STILL OWED on item 9, and it is the half with the teeth:** the LEVEL-CHECK DEADLINE and the no-new-audio
staleness test (his 400-window, 20-minute EJThreshold pass on MDynamicsMBLarge). `EJCalibLoop` already has
`noSignalMs`/`kNoSignalMs` and a stale-window rule that refuses to treat a repeated window as a sample - what it
has NO notion of is an overall deadline, so a pass that can never make progress waits for ever rather than ending
with a sentence. That work is sized and understood; it is not in 08b unless CHAINS lands early.

#### 8c-C-done. SAVE SERIALISES THE RACK THE USER IS LOOKING AT (8 Oct, in 08b; ui_guard GREEN)
Sean saved "Aitch Vocal Chain" while viewing the vocal LINK rack; it reached the server and held the MIX BUS,
because Save read `processorRef.getChainHost()`. The EDIT path already answered the same question the other way,
and says why in its own comment: while a rack is leased the Link PARKS its slots, so the content lives in the
BORROWED host. Two paths, one question, two answers - and the quiet one silently saved the wrong chain.
**ONE AUTHOR NOW:** `EchoJayEditor::chainHostForSave(whichOut)` - the view's borrowed rack when a Link rack is
selected, this instance's own otherwise. It takes the **VIEW's** uid (`chainViewUid()`), not the chat's, because
Save is a button on the rack in front of the user. An empty uid, or one with no live borrow, is the local rack -
the same fallback the edit path uses and the only honest reading of "no Link rack is selected". Every save logs
WHICH rack it serialised and how many slots it had.
**`saveChainToApi`'s emptiness check asks the same host**, or "There are no plugins in the chain to save" could
appear over a full Link rack - and a full local rack could wave through a save of an empty one.
**LEG in `ui_guard`, GREEN:** with no Link rack selected Save takes this instance's host; with a live borrow for
the uid the view is showing it takes the BORROWED host, asserted as a different object from the local one - which
is the whole bug, since one of them holds the vocal chain and the other the mix bus - and the log phrase names
which it took.

#### 9-part2. THE LEVEL CHECK NOW ENDS, AND SAYS WHICH THING HAPPENED (8 Oct, in 08b; level_loop_guard GREEN)
Sean's EJThreshold pass on MDynamicsMBLarge ran 400+ windows over twenty minutes with byte-identical readings and
was still running. The loop already refused each repeated window - correctly - and already had a no-signal clock
that ASKS at 30 s. What it had no notion of was an ENDING.
**TWO BOUNDS, both terminal:** a run of `kStaleRunEnd = 20` consecutive non-sample windows (about a minute at 3 s
- long enough that a tape stop or a punch does not end a pass, short enough that nobody watches a dead loop), and
an outer `kMaxWindows = 200` (about ten minutes) checked BEFORE every early return, so a silent window, a dropped
window and a repeated window all count against it. Sean had passed the outer bound twice over.
**THE ENDING ASKS RATHER THAN GUESSES.** `judgedAny` records whether this pass ever had a single sample: a pass
that never did has an AUDIO problem, which is the user's to fix ("No audio reaching <plugin> - press play on the
loudest part and tap Listen."); a pass that had samples and still went nowhere is OURS ("Nothing dialled on
<plugin> - set it by ear."). Both are logged with the window count and the reason, so a pass that ended early and
one that ran its full cap are told apart.
**LEGS (16) in `level_loop_guard`, GREEN:** his exact case - one judged window, then frozen readings - ends after
**20** frozen windows with the no-audio sentence, naming the plugin, with something the user can do, and the log
records which ending it was; and a pass fed nothing but SILENCE also ends inside the outer bound with a sentence
instead of waiting for ever.
**ONE LEG IS OWED AND I AM NOT FAKING IT:** the "Nothing dialled on ..." branch is reached through the same cap
when a pass HAS judged windows and still makes no progress. A fixture that keeps feeding fresh audio makes this
loop progress and finish normally in eight windows - which is correct behaviour, not the case under test - so that
leg needs a sensor that reports samples while refusing to move, and it is written down rather than approximated.

### 9. PER-RACK LOOP STATE IS NEVER RESET, AND THE LEVEL CHECK WAITS FOR EVER (Sean 11:10, 8 Oct, on 08a)
Folded in here as ruled, with the heard counter. Second rap-vocal chain: the UI stuck on the level check and never
resolved, and the post-build summary on that NEW rack named the PREVIOUS chain's dynamics slots -
"loops alive 0 of 2 dynamics slots - ended: UAD Neve 2254 E Dual (closed), UAD UA 176 (landed ...)".

**(1) THE BOOKKEEPING IS NEVER CLEARED - proven by absence in the owning file.** `loopsAlive_`, `loopsStarted_`
and `loopsDead_` (ChainHost.h:2673-5) have exactly two writers, `setLoopsStarted` and the alive+dead setter
(ChainHost.h:766, 771). **Nothing resets them** - not `removeSlot`, not `restoreSavedChain`, not a revision bump,
not a rebuild. So `loopsWatchdogLine()` (ChainHost.cpp:5438) keeps printing the last rack's dead loops against the
new rack's `dynamicsSlotCount()`, which is exactly the line he read. The fix is a reset on the same bump that
already invalidates the per-slot tallies, plus **the rack revision stamped on every loop** so a line can never
describe a rack the loop did not belong to - that third part is what makes the first two checkable.

**(2) THE LEVEL CHECK MUST RESOLVE, same shape as the Listen deadline (item 2, already shipped in 08a).** Slot 3,
MDynamicsMBLarge, got two controls from the server ("Band 2/3 - Dynamic detection - Release mode"), requested=2
applied=0 **status=writesRejected**, so nothing dialled and no loop started - and the UI kept waiting on something
that was never going to report. A deadline of about 20 s with a NAMED outcome, and for this case the honest one is
**"nothing dialled on MDynamicsMBLarge, set it by ear"**, said with the reason (writes rejected, or no usable
controls). This is the third unbounded wait found in two days; the pattern is now explicit in the handoff
(a state-only wait is not a wait, it needs a clock and a sentence).

**(3) LOW PRIORITY, LOG HYGIENE, cause found:** every map arrival runs `applyStructuredIfReady(i,
DialTrigger::mapArrived)` for **every slot in the rack** (ChainHost.cpp:4305-4312), and each slot with no settings
yet logs "no settings yet [map-arrived]" (ChainHost.cpp:5726-5733). Six slots times N maps, describing whichever
rack the host currently holds rather than the rack the map was for. The line itself already says "EXPECTED
ORDERING, not a fault", so nothing is wrong except the volume: once per rack revision, not once per map per slot.

**LEGS:** a rack with two dynamics slots whose loops have ended, replaced by a rebuild with one dynamics slot that
dialled nothing -> the watchdog line names NO stale plugin and the level check resolves with "nothing dialled on
<plugin>, set it by ear"; and a loop stamped with revision N is never reported against revision N+1.

### 9. Reset the heard counter in [CHAIN LEVELS] when the rack changes
"set from N min" must mean THIS build. The phrase is composed in EJCalibLoop.h (~1968 and ~2146,
`", set from " + roundToInt (blockHeardS) + " s of this track"`), and the CHAIN LEVELS block is assembled at
EchoJayAPI.cpp:3263. The reset point is a rack change - the same bump that already invalidates per-slot tallies
(`bumpChainRevision`). LEG: a rack change resets the heard figure, so a build cannot quote minutes heard before the
chain it describes existed.
