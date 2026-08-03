# Running several Claude Code sessions at once — use git worktrees

## The problem we hit
Two Claude Code sessions in the **same folder** share one git HEAD and one
working tree. When one runs `git checkout`, it moves the other session's branch
too — so commits land on the wrong branch (this happened during EQ Phase 1 +
Wave 0). Self-registration prevents *content* conflicts; it does nothing about
this, because it's a filesystem-level collision underneath git.

## The fix: one folder per session (git worktrees)
A worktree is a second working directory backed by the **same** repo/`.git`
object store. Each worktree has its **own** HEAD and checked-out branch, so
sessions never move each other's HEAD. No extra clones, no duplicated history.

Run each Claude Code session from its **own** worktree folder.

### Setup (run once, from the main repo `~/echojay-vst`)
```bash
cd ~/echojay-vst
git checkout feat/surgical-eq && git pull        # main folder stays on surgical-eq

# Wave 0 (framework) — branch already exists, sits on top of Phase 1:
git worktree add ../ej-framework feat/builtin-framework

# Wave 1 clusters — each a NEW branch off surgical-eq, its own folder:
git worktree add ../ej-stereo   -b feat/builtin-stereo   feat/surgical-eq
git worktree add ../ej-mod      -b feat/builtin-mod      feat/surgical-eq
git worktree add ../ej-dynamics -b feat/builtin-dynamics feat/surgical-eq
git worktree add ../ej-harmonic -b feat/builtin-harmonic feat/surgical-eq
git worktree add ../ej-time     -b feat/builtin-time     feat/surgical-eq
```

### Run a session in its worktree
```bash
cd ../ej-dynamics && claude      # this session is pinned to feat/builtin-dynamics
```
Each session `cd`s into its own `ej-*` folder and launches `claude` there. They
build, commit, and push independently. `git worktree list` shows them all.

### Merging back
When a cluster is green and pushed, merge its branch into `feat/surgical-eq`
one at a time (from the main folder). Because each device self-registers in its
own files, these merge clean. Remove a finished worktree with
`git worktree remove ../ej-dynamics`.

## One shared resource: the installed AU  (READ THIS — it already cost us a day)
All seven worktrees build the **same plugin identifier** (`com.echojay.plugin.v2`)
to the **same install path** (`~/Library/Audio/Plug-Ins/Components/EchoJay V2.component`).
So "reinstall the AU" means "overwrite Logic's plugin with whichever worktree ran
last." This produced a full day of ghost-chasing: a correct editor-crash fix on
`feat/surgical-eq` was silently overwritten by an `ej-time` build made six minutes
earlier on a branch without the fix — so Logic kept crashing on a fix that was
never once loaded, while the headless harness (built from fixed source) kept
passing. Both true at once, and indistinguishable without checking the installed
binary's UUID.

**The rule, to never repeat this:**
- **Cluster sessions do NOT install to the system.** They build in their own
  `build/` tree and self-test there: the g++ tests, the registry/editor harness,
  and `pluginval` run against the worktree's *own* build artifact path. They must
  NOT copy anything into `~/Library/Audio/Plug-Ins/`. (Amend the "rebuild +
  reinstall the AU" line in the cluster prompts to "rebuild + run the harness +
  pluginval on this worktree's build — do NOT install to the system.")
- **Only the integration worktree installs for DAW testing.** From `~/echojay-vst`
  on `feat/surgical-eq`, after merging a cluster in, install with
  `tools/install_local.sh` — it installs THIS worktree's build, re-reads the UUID
  off what it wrote, clears `~/Library/Caches/AudioUnitCache`, and flags any
  root-owned `/Library` shadow copies.
- **Before trusting any DAW test, confirm what's loaded.** `tools/which_build_is_installed.sh`
  prints the installed UUID and names the worktree/branch/commit it came from —
  the same UUID a crash report prints. If a host ever "ignores your fix," run this
  FIRST; do not re-debug the fix.
- **A cluster must carry the crash fix before it is ever installed/DAW-tested.**
  Merge `feat/surgical-eq` (≥ `524c4ff`) into the cluster branch first, or its
  build reinstalls the pure-virtual editor crash.
- **`pluginval` is not a full gate for inline-device bugs:** it exercises the
  plugin's *top-level* editor, not `ChainListPanel::showInline` opening a built-in
  device editor inside the rack — the path this crash lived on. Trust the editor
  harness (which now parents + sizes an editor the way the rack does) for that.
- Watch for **root-owned `/Library` shadows** (both `.component` and `.vst3`) with
  the same identifier — they override the user build nondeterministically. Remove
  with `sudo rm -rf`.

### This has already cost us an afternoon — verify, don't assume
The paragraph above is not hypothetical. The built-in device editors aborted
Logic on open (`__cxa_pure_virtual` out of `DeviceEditorBase`'s constructor).
The bug was found, fixed, committed, tested green and reinstalled — and Logic
kept crashing in exactly the same place, for hours, because what it had loaded
was a Release build from `../ej-time`, made six minutes BEFORE the fix and on a
branch that does not contain it. Every worktree ships the same
`CFBundleIdentifier` to the same path, so "reinstall" silently means "overwrite
with whichever worktree went last", and a fix that never loaded is
indistinguishable from a fix that does not work. The headless harness passed
throughout — correctly, because it was built *here*, from fixed source.

The lesson is not "be careful". It is that **which build is installed is a
question with a checkable answer**, and it should be asked before re-debugging
anything that appears not to be fixed:

```bash
tools/which_build_is_installed.sh   # whose binary will a host actually load?
tools/install_local.sh              # install THIS worktree's build, then prove it
```

`which_build_is_installed.sh` prints the installed bundle's Mach-O UUID and then
names the worktree and branch whose build tree that UUID came from. A crash
report lists the same UUID for the loaded image, so the two can be compared
directly — which turns "is the DAW even running my code?" from an afternoon into
one command.

## Rule of thumb
- **1 session = 1 worktree folder = 1 branch.** Never two sessions in one folder.
- Main folder `~/echojay-vst` stays on `feat/surgical-eq` (integration branch).
- Clusters are independent; merge them back one at a time.
