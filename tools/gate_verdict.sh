#!/bin/bash
# =============================================================================================
#  gate_verdict.sh - WRITE DOWN WHAT THE WHOLE SUITE SAID, BESIDE THE BUNDLES IT SAID IT ABOUT
#  (C's point 80, 29 Oct 2026, accepted)
# =============================================================================================
#  What happened: mapfps_test was RED for thirteen days and nobody saw it, because every round
#  since 16 Oct ran "touched guards only" - the right policy for a fast loop - and that suite was
#  never touched. So "touched guards: GREEN" stood in for "the build is green" at every install in
#  between. C's words: the same shape as a green leg against a stub.
#
#  So an install can no longer CLAIM green on its own. This writes a verdict file next to the
#  bundles, naming every suite that ran, its counts, the commit and the time; install_local.sh
#  refuses to install bundles that have no verdict, whose verdict is RED, or whose verdict is older
#  than the bundles themselves (a verdict about an earlier build is about an earlier build).
#
#  Usage:  tools/gate_verdict.sh <build-dir> <GREEN|RED> "<suite>: <result>" ["<suite>: <result>" ...]
#
#  It records what it is TOLD, by the round that ran the suites - it does not re-run them and does
#  not judge. A round that leaves a suite out must say so in its own line, because "not run" and
#  "passed" are the two things this file exists to keep apart.
# =============================================================================================
set -u
BUILD_DIR="${1:-}"
VERDICT="${2:-}"
shift 2 2>/dev/null || true
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -z "$BUILD_DIR" || -z "$VERDICT" ]] && { echo "usage: gate_verdict.sh <build-dir> <GREEN|RED> \"<suite>: <result>\"..."; exit 2; }
case "$VERDICT" in GREEN|RED) ;; *) echo "gate_verdict: the verdict must be GREEN or RED, not '$VERDICT'"; exit 2;; esac
D="$ROOT/$BUILD_DIR"
[[ -d "$D" ]] || { echo "gate_verdict: no such build dir: $D"; exit 2; }

# ---- WHICH SOURCE THIS VERDICT IS ABOUT (C's point 82, 29 Oct 2026) -----------------------
#
# The install checked that the verdict exists, is GREEN, and is not older than the binary. It
# never checked that it is about the same SOURCE - and the 15:42 verdict shows why that matters:
# it recorded "commit 82e46d99, tree 31 uncommitted". A verdict written from a gate run on a
# different tree (another worktree, a stash, a re-run after an edit) passes a timestamp test and
# says nothing true about the bytes in the bundle.
#
# So the verdict carries a FINGERPRINT of the compiled sources: every file under Source/ that goes
# into a plugin, hashed by content, in a stable order. Not `git stash create` - that describes the
# index and the worktree, and the thing being gated is the files the compiler read. Untracked
# headers (there were three today) count exactly like tracked ones, which is the whole point.
SRC_FP="$(cd "$ROOT" && find Source -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.mm' \) \
            -print0 2>/dev/null | sort -z | xargs -0 shasum -a 256 2>/dev/null \
            | shasum -a 256 | cut -c1-16)"

OUT="$D/GATE_VERDICT.txt"
{
  echo "verdict: $VERDICT"
  echo "written: $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "commit:  $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "branch:  $(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
  echo "sources: $SRC_FP   (sha256 of every Source/*.cpp,*.h,*.mm by content)"
  echo "tree:    $(git -C "$ROOT" status --porcelain 2>/dev/null | wc -l | tr -d ' ') file(s) uncommitted"
  echo "suites:"
  if [[ $# -eq 0 ]]; then
    echo "  (none named - which is itself a red flag: a verdict with no suites behind it)"
  else
    for s in "$@"; do echo "  $s"; done
  fi
} > "$OUT"
echo "gate_verdict: $VERDICT written to $OUT"
cat "$OUT"
