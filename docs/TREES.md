# Which tree is authoritative for what

Written 8 Aug 2026, immediately after installing a plugin **two minor versions
behind** over the working one. The build succeeded, the artefact was correct,
the SHA-256 of the copy matched its source — every check passed, and the
binary was still wrong, because the question nobody asked was *which tree
ships this file*.

**Read this before editing any file that ends up in a shipped binary.**

---

## The trees

| checkout | branch | owns | build dir | plugin version |
|---|---|---|---|---|
| `echojay-vst-v200` | `feat/plugin-dashboard` | **THE SHIPPING PLUGIN** | **`build-dev`** | 2.25.23 |
| `ejmap-wt` | `feat/ejmap` | the ejmap mapper tool + corpus tooling | `build-ejmap` | 2.23.21 ⚠ |
| `echojay-saas-dialable` | `pricing-v2` | **THE SHIPPING SERVER** | — (Vercel) | — |
| `echojay-saas` + 6 siblings | various | feature worktrees, not deployed | — | — |

### The two traps

**1. `ejmap-wt` contains a full plugin source and it is stale.** `Source/` is
there, it compiles, `cmake --build build --target EchoJay_AU` produces a
perfectly good `EchoJay V2.component` — at **2.23.21**, two minor versions
behind. `Source/ChainHost.cpp` differs from the shipping one by **1,033 lines**.
Nothing in that tree should ever be built as a plugin or installed.

> Edit `Source/*` in `ejmap-wt` **only** for files the ejmap tool itself
> compiles. If a change is meant to reach a DAW, it belongs in
> `echojay-vst-v200`.

**2. The pre-commit gate in `ejmap-wt` does not build the plugin.** It builds
`ejmap-roundtrip-test` and runs `audit_maps`. It stayed green through a plugin
edit, a plugin build and a plugin install, because it has no opinion about
`EchoJay`. **A gate that does not build the artefact you ship cannot tell you
that you built the wrong one.**

### Divergence runs both ways

`feat/ejmap` is not simply "behind". Work exists on each branch that the other
lacks:

- **only on `feat/plugin-dashboard`**: the `set` edit op (`4801ee8`), remote
  editing, the EQ curve publish, everything since 3 Aug.
- **only on `feat/ejmap`**: `ChainHost::rackedControlSurface()` — the client
  half of the CONTROLS injection, which is why `CONTROLS_MIN_PLUGIN_VERSION`
  cannot simply be lowered: the shipping client never sends a control surface.

So "merge the newer one" is not a safe reflex in either direction. Check what
each side has before assuming which is ahead.

---

## Installing a plugin build

`CMakeLists.txt` sets `COPY_PLUGIN_AFTER_BUILD FALSE`, so **the build never
installs.** There is no `reinstall-v2.sh` — that script does not exist, despite
being remembered. The copy is manual:

```bash
cd ~/Documents/"ECHOJAY FILES"/"ECHOJAY VST"/echojay-vst-v200
cmake --build build-dev --target EchoJay_AU

SRC="build-dev/EchoJay_artefacts/Release/AU/EchoJay V2.component"
DST=~/Library/Audio/Plug-Ins/Components/"EchoJay V2.component"
# Quit the DAW first: replacing a loaded component gives you a new mtime and
# the old code.
rm -rf "$DST" && cp -R "$SRC" "$DST"
```

The dev install is **user-domain** (`~/Library/...`). Nothing is installed in
`/Library/...`, so there is no shadowing copy — if one ever appears, resolve it
before debugging anything else.

### Verify the install, in this order

```bash
defaults read "$DST/Contents/Info.plist" CFBundleShortVersionString   # THE version check
shasum -a 256 "$SRC/Contents/MacOS/EchoJay V2" "$DST/Contents/MacOS/EchoJay V2"
auval -v aufx EcJ2 Ecjy | tail -3                                     # expect PASS
```

**The version string is the check that would have caught this**, and it is the
one I skipped. A matching SHA proves the copy is faithful; it says nothing
about whether the source was the right source. Faithfulness and correctness are
different properties and only one of them was being tested.

`strings | grep -c` is a weak check: string literals are shared between call
sites and a universal binary carries two slices, so counts do not map to call
counts. Grep for a distinctive fragment and expect **2** (arm64 + x86_64).

---

## Server-side gates that make the plugin look broken

`api/chat.js` withholds capabilities from clients below a minimum version,
judged on the `appVersion` the plugin declares (`EchoJayAPI.cpp:1405`,
`JucePlugin_VersionString`). All three sit at the sentinel `2.99.0` — above
every shipped version — pending release of the client half:

```
BANDS_MIN_PLUGIN_VERSION    = '2.99.0'   client half PRESENT at 2.25.23
CONTROLS_MIN_PLUGIN_VERSION = '2.99.0'   client half ABSENT — lives on feat/ejmap
SET_OP_MIN_PLUGIN_VERSION   = '2.99.0'   client half PRESENT at 2.25.23
```

**There is no per-user or per-account override.** The only input is the
client-declared version, so a local build that declares a higher version is the
only way to open a gate for one machine.

A gated client is never *taught* that `set` exists, so the model reaches for
`replace` — the exact defect the set op was written to fix. From the outside
this is indistinguishable from the model choosing badly, which is why
`ChainHost::applyStructuredIfReady` now logs `appVersion` on the NO SETTINGS
path.
