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

## One shared resource: the installed AU
All worktrees build and **install to the same** `~/Library/Audio/Plug-Ins/
Components/`. Building in parallel is fine, but installing/testing in Logic is
one-at-a-time — whichever session reinstalls last is the AU Logic loads. So:
parallelize the *building*; serialize the *"reinstall + open Logic to test"*
step. (Or give each build a distinct product name while iterating.)

## Rule of thumb
- **1 session = 1 worktree folder = 1 branch.** Never two sessions in one folder.
- Main folder `~/echojay-vst` stays on `feat/surgical-eq` (integration branch).
- Clusters are independent; merge them back one at a time.
