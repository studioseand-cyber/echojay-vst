# ejmap M0 + M1: Claude Code execution brief

**Repo:** `echojay-vst-v200`
**Branch:** `feat/ejmap`, cut from `feat/param-band-matcher`
**Companion:** `EJMAP_IMPLEMENTATION_PLAN.md` (full 13 module plan)

This brief covers the first two modules only. Do not start M2 until the M1 gate
passes with a measured number.

---

## 1. Setup, before touching any file

Branch choice matters. `feat/param-band-matcher` carries the band matcher,
bidirectional interpolation, segment interpolation and the read-back comparator.
ejmap needs all four, and needs the same ones EchoJay will ship. Cutting from
`v2` gets a mapper that verifies against logic the client does not have.

```bash
cd "~/Documents/ECHOJAY FILES/ECHOJAY VST/echojay-vst-v200"

# The 30s TTL override in ChainHost.cpp must not be carried into this branch.
git status
git checkout Source/ChainHost.cpp     # if it shows as modified

git worktree add "../ejmap-wt" -b feat/ejmap feat/param-band-matcher
cd "../ejmap-wt"
```

**Discipline for this branch, all four of these have bitten before:**

- One Claude Code session in `ejmap-wt`, never shared with another session
- Build directory is `build-ejmap`, never `build`
- ejmap installs nowhere near the AU component path, so it cannot collide with a
  plugin build in either direction
- The title bar shows version plus git short hash. That is the only proof of what
  is running

---

## 2. Files provided

Drop these in unchanged, then wire the seams in section 3.

```
tools/ejmap/CMakeLists.txt
tools/ejmap/Source/EjmapSchema.h        payload v2.1 types, enums, JSON
tools/ejmap/Source/EjmapLedger.h        ledger, inflight protocol, quarantine
tools/ejmap/Source/PluginScanner.h
tools/ejmap/Source/PluginScanner.cpp    AU registry walk, VST3 walk
tools/ejmap/Source/PluginHost.h
tools/ejmap/Source/PluginHost.cpp       instantiate, silent pump, editor
tools/ejmap/Source/MainComponent.h      M1 shell
tools/ejmap/Source/Main.cpp
tools/ejmap/tests/RoundTripTest.cpp     drift guard
```

Add to the root `CMakeLists.txt`:

```cmake
add_subdirectory(tools/ejmap)
```

Build:

```bash
cmake -B build-ejmap -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-ejmap --target ejmap -j8
cmake --build build-ejmap --target ejmap-roundtrip-test -j8
```

---

## 3. Seams to wire

Three, and none of them should be guessed at. Read the real headers first.

### 3.1 `kMapSchemaVersion` moves into the shared header

Currently it lives in `EjmapSchema.h`. It must live in `Source/EchoJayParamApply.h`
so both binaries reference one constant, with a `static_assert` on each side.

Do this: add the constant to `EchoJayParamApply.h`, have `EjmapSchema.h` include
that header and static_assert equality, then delete the local definition.

**Verify it works by breaking it.** Change the constant in the shared header,
confirm both `ejmap` and the EchoJay plugin target fail to build, then revert.
A drift guard that has never been observed failing is not a guard.

### 3.2 The AU registry walk

`PluginScanner.cpp` contains a reference implementation using
`AudioComponentFindNext`. It is written from the outside.

`tools/ejextract` already has the proven version behind `--au-registry`, the one
that found the 701 components the file walk missed. **That one wins.** Read
`tools/ejextract/EchoJayParamExtractor.h` and whatever drives `--au-registry`,
and replace the body of `PluginScanner::scanAudioUnits` with a call into it.

Specifically check `createAuIdentifier`: the identifier string form is my
recollection of what JUCE's `AudioUnitPluginFormat` expects. If ejextract or the
JUCE source in this repo disagrees, they are right and that function is wrong.

Two implementations of enumeration will drift, and enumeration drift is how the
Waves shells went invisible.

### 3.3 The round trip itself

`tests/RoundTripTest.cpp` has six real checks that pass today plus one
`TODO(fable)` for the actual round trip. Wire it:

1. Read a map from `~/Library/ejmap/maps/`
2. Build the settings block the mapper verified, from `evidence.readback`
   (semantic to `wrote` value)
3. Run it through the real `applySettings` in `Source/EchoJayParamApply.h`
4. Assert the resulting writes, index and normalised value, are identical to what
   the evidence recorded

**Do not reimplement interpolation inside the test.** Reimplementing it is
exactly the drift this test exists to catch.

The corpus is empty until M1 produces maps, so this stays skipped and prints why.
That is correct: a skip that announces itself is fine, a skip that looks like a
pass is the silent-drop class.

---

## 4. M0 gate

| Check | Pass condition |
|---|---|
| Target builds | `ejmap` links and launches an empty window |
| Version visible | Title bar shows version, git short hash and schema string |
| Drift guard live | Break a shared function signature, confirm both binaries fail to build, revert |
| Test runs | `ejmap-roundtrip-test` exits 0 with 6+ checks and an announced corpus skip |

---

## 5. M1 gate

Measured numbers, against stored data. Not estimates.

| Check | Pass condition |
|---|---|
| Enumeration | Roughly 1,536 non-instrument entries, roughly 1,267 distinct products on this machine. This is the one estimate in the whole project that survived verification, so a large deviation means the walk is wrong, not that the number moved |
| Errors surfaced | `~/Library/ejmap/scan-errors.log` written, every enumeration failure named |
| Editors open | 20 plugins across formats and vendors, including one Waves shell component, one Plugin Alliance, one Valhalla |
| Outcomes stored | `ledger.json` has one line per attempt with a real outcome from the vocabulary |
| Crash recovery | Force-quit the app mid-load, relaunch, confirm the plugin is marked `crash_on_load` and quarantined, and the app comes up |
| Counts from disk | The outcome summary is read back out of `ledger.json`, never from an in-memory tally |

That last row is the rule this project adopted after the tripwire bug survived
its own test: assert reported numbers against stored data, never against the
code's intent.

---

## 6. Known rough edges in the provided code

Flagged so they are not discovered as surprises.

- **`createAuIdentifier`** is unverified. See 3.2.
- **Editor scaling.** `layoutEditor` deliberately does not scale or transform the
  hosted editor. M2 records mouse position inside those bounds and a transform
  makes every `ui_hint` coordinate a lie. If a plugin editor is larger than the
  holder, add scrolling, never scaling.
- **The silent pump** runs `processBlock` on a worker thread at roughly block
  rate. M9's offline probe render must call `setPumpEnabled(false)` and take
  `processLock` before rendering, or the two will interleave.
- **Licence detection** in `PluginHost::load` sniffs the error string. It is a
  heuristic. If ejextract has a better signal for `license_refused`, use that.
- **`noParams` leaves the instance loaded** on purpose, so the human can still
  look at the editor and confirm the plugin genuinely exposes nothing. That is a
  permanent, honest result, not an error.
- **Quarantine release is manual** by design. A plugin that crashed once usually
  crashes again, and auto-retry turns one lost session into a loop.

---

## 7. What comes next, and what not to start

M2 is the capture engine: 30 Hz poll, noise mask baseline, listener layer as
disambiguator, and the mouse-position capture that produces `ui_hint`. It is the
first module where the design is load-bearing rather than plumbing.

**Do not start it until the M1 gate passes with real numbers.** If enumeration is
wrong, every capture sits on top of a wrong plugin list.

Do not build any of these yet, even though the schema has slots for them: audio
probe, AI assist, upload, groups, Tier 2. The schema carries their fields so the
payload does not need a migration later. The code comes in phase order.
