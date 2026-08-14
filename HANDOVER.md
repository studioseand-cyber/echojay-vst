# HANDOVER — plugin work, 10 Aug 2026

For any Claude Code session opening any tree of this repo. Read this before
editing anything. Trees and their jobs: `ejmap-wt/docs/TREES.md`. Dial-test
rig and preview endpoints: `ejmap-wt/docs/DIAL_TEST_RIG.md`.

## 1. The authoritative line

**feat/plugin-dashboard @ `2d5ad04`** (version 2.26.0). Everything branches
from or rebases onto this. It already contains — do not re-implement:

- `1d42d89` — the dial batch: mapFps (per-fp exact controls exposure), the
  honest surfaces (card-only edit turns, never-relay result composition,
  receipt-time suggestion consumption, reason-on-card, empty-bubble skip),
  card lines rendered from the structured payload, EJDial failure-class logs.
- `69552f2` / `385636d` / `9de8317` — merges of v2 (the two DEFECT filings),
  classifier, and stream. Stream extracted the chat-reply pipeline into
  `handleChatReply`; the batch's hunks are PORTED into it. Do not revert the
  extraction; edit the pipeline in `handleChatReply`, nowhere else.
- `2fb3823` — ladder-tolerance proof: `typedReadbackMatch` nearest-step (fix
  itself landed 1 Aug) pinned on XTComp's served 8-step ratio tables.
- `389f78c` — bridged-AU report-only: `EchoJayBridgedAU.h` detection +
  `applyOne` demotion on all four verify paths, proven live on API-2500
  (`tools/bridged_readback_test`). On a bridged AU, EVERY in-stack read is
  pre-write — display and getValue() both, measured.
- `2d5ad04` — 2.26.0. Unsigned installer pkg at repo root; leave it.

Yesterday's ejmap/decline/sanitizer work lives on **feat/ejmap** (`f685bc9`
declines, `876a054` stepped sanitizer, `d6b8959` fresh-session) and is
deliberately NOT merged here — see §4 before touching either side's shared
Source files.

## 2. Do not touch

- **The 2.99.99 version bump is gone from history and must not come back.**
  It existed only to open the server dial gates for one local test binary.
- **The server gates are pinned at 2.99.0 until Sean deploys the 2.26.0
  pins.** Until then a 2.26.0 (or any real-version) build does NOT dial:
  no set op is taught, `[CONTROLS]` is not served. That is the gate working,
  not a regression — do not "fix" it client-side, do not bump the version.
- Only Sean deploys anything (server or signing). Never run vercel; the
  installer waits for his productsign/notarize.

## 3. Rules that were paid for this week

- **Check which tree ships a file before editing it.** Shared Source/ files
  exist on multiple branches in different states; `git worktree list` first.
- **Verify artefact identity, never filename or version string.** The first
  2.26.00-named pkg contained 2.25.23 binaries: `build-installer.sh` only
  packages `build/`, and versions land in Info.plist at cmake CONFIGURE
  time. Reconfigure, rebuild, then check the plist INSIDE the artefact.
- **A filing names the commit it was diagnosed against.** Both DEFECT_*.md
  files were written on a branch 297 commits behind; one described a defect
  the mainline had fixed the day before. Check the described code exists at
  YOUR branch before building the fix.
- **`~/.echojay/dev.json` overrides the API endpoint entirely.** A stale
  pinned preview serves old server code silently — five deploys were once
  graded "not working" against a frozen preview. Check what a test is
  actually served before diagnosing (probe recipe in DIAL_TEST_RIG.md).
- **Assert on content, not completion.** A green run with empty output is
  the house failure shape (153 "mapped" plugins that swept nothing).

## 4. Deferred, and where it is written

`docs/EJMAP_MERGE_QUEUE.md` holds all of it: the feat/ejmap merge (the
silent-returns twin is a hunk-by-hunk reconciliation, NOT a merge), the
bipolar-pan signed parse (build-side + client readback must land together),
and the full bridged stable-read verification that the report-only
mitigation stands in for. `DEFECT_BRIDGED_READBACK.md` carries the
mechanism; its setread-immunity claim is measured false (see §1, 389f78c).

## 5. First thing on opening a tree

```
git worktree list                       # know which tree you are in
git status --short                      # someone else's uncommitted work? STOP, ask
git rev-list --left-right --count feat/plugin-dashboard...HEAD
```

Second number > 0: your branch has commits dashboard lacks — fine, carry on.
First number > 0: you are behind — rebase onto (or merge) feat/plugin-dashboard
BEFORE touching Source/. If your tree holds uncommitted changes you did not
make, do not commit, stash, or clean them — name them to Sean and wait.
