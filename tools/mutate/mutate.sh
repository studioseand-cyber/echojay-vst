#!/bin/bash
# ---------------------------------------------------------------------------
# MUTATION HARNESS: apply a mutation, run the gate, RESTORE WHATEVER HAPPENS.
#
#   tools/mutate/mutate.sh <file-to-mutate> <mutation.py> [mutation.py ...]
#
# Each mutation.py edits the target in place and must assert its own anchor, so
# a mutation that does not apply says so instead of producing a silent no-op
# green. Each is run against a PRISTINE copy: they do not stack.
#
# EACH PASS PRINTS ONE OF THREE VERDICTS, and INCONCLUSIVE is the default:
#
#   REDDENED      the suite ran and reported FAIL (   the pins saw it
#   SURVIVED      the suite ran and reported PASS (   the pins are blind
#   INCONCLUSIVE  no verdict line was parsed          nothing was measured
#
# The only way out of INCONCLUSIVE is an affirmative PASS ( or FAIL ( line from
# the suite itself. A build failure, a missing binary, an empty log or a log
# with no verdict line all stay INCONCLUSIVE and say which.
#
# ---------------------------------------------------------------------------
# WHY THIS FILE EXISTS, WHICH IS THE WHOLE POINT
# ---------------------------------------------------------------------------
# Every mutation run in this project was an ad-hoc script in /tmp, rewritten
# from scratch each time. Eight of them, none with a trap. A kill therefore
# skipped the restore and LEFT THE MUTATION IN THE WORKING TREE: four times in
# one session, and every time it was caught by comparing a hash afterwards,
# which depends on somebody remembering to compare.
#
# The trap below is what turns "caught it four times" into "cannot happen".
# It was asked for two days before it landed, and the reason it did not land is
# that there was no durable script to put it in. That is the actual defect this
# file fixes; the trap is one line of it.
#
# PIPE IS IN THE TRAP LIST ON PURPOSE. A SIGPIPE skips EXIT traps, which this
# repository has already paid for once: a build lock outlived its holder that
# way. Piping this script's output into `head` is enough to do it.
#
# SIGKILL CANNOT BE TRAPPED. kill -9, a machine losing power, or an OOM kill
# will still leave the mutation in place, so the final verification below is
# kept as a backstop rather than deleted as redundant. The difference is that it
# now runs automatically and fails loudly, instead of being a step a person
# has to remember.
#
# NO GIT IN THIS SCRIPT, by standing rule. It touches one file and restores it.
# ---------------------------------------------------------------------------
set -u

TARGET="${1:-}"
shift || true
if [ -z "$TARGET" ] || [ $# -eq 0 ]; then
  echo "usage: mutate.sh <file-to-mutate> <mutation.py> [mutation.py ...]" >&2
  exit 2
fi
[ -f "$TARGET" ] || { echo "no such file: $TARGET" >&2; exit 2; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNNER="$REPO/tools/mapfps_test/build_and_run.sh"
[ -x "$RUNNER" ] || { echo "no runner at $RUNNER" >&2; exit 2; }

PRISTINE="$(mktemp -t ej_mutate_pristine)"
cp "$TARGET" "$PRISTINE"
BASELINE="$(shasum -a 256 "$TARGET" | cut -d' ' -f1)"

restored_ok=0
restore () {
  # Idempotent, and SILENT ABOUT SUCCESS on the ordinary path so the trap firing
  # at normal exit does not add noise to a clean run.
  cp "$PRISTINE" "$TARGET" 2>/dev/null
  local now
  now="$(shasum -a 256 "$TARGET" 2>/dev/null | cut -d' ' -f1)"
  if [ "$now" != "$BASELINE" ]; then
    echo "!! RESTORE FAILED: $TARGET is $now, baseline $BASELINE" >&2
    restored_ok=0
  else
    restored_ok=1
  fi
}
# THE LINE THIS FILE EXISTS FOR.
trap 'restore' EXIT INT TERM PIPE

echo "=== MUTATION RUN: $TARGET"
echo "=== baseline $BASELINE"

for MUT in "$@"; do
  echo ""
  echo "########## $(basename "$MUT") ##########"
  cp "$PRISTINE" "$TARGET"

  if ! python3 "$MUT"; then
    echo "VERDICT: INCONCLUSIVE (the mutation did not apply)"
    cp "$PRISTINE" "$TARGET"
    continue
  fi

  if cmp -s "$PRISTINE" "$TARGET"; then
    echo "VERDICT: INCONCLUSIVE (applied, but the file is unchanged)"
    cp "$PRISTINE" "$TARGET"
    continue
  fi

  OUT="$(mktemp -t ej_mutate_run)"
  "$RUNNER" > "$OUT" 2>&1
  RUNNER_RC=$?

  grep -E '^  FAIL' "$OUT" | sort -u
  grep -E '^(PASS|FAIL)  \(' "$OUT"

  # ------------------------------------------------------------------------
  # THREE OUTCOMES, AND INCONCLUSIVE IS THE DEFAULT.
  #
  # The run leaves INCONCLUSIVE only by PARSING AN AFFIRMATIVE VERDICT LINE
  # out of the suite's own output. Nothing else promotes it: not the runner's
  # exit code, not the absence of failures, not the log existing.
  #
  # WHY THE DEFAULT MATTERS, and it is not the full disk that made this
  # visible. THE DANGEROUS CASE IS A MUTATION THAT DOES NOT COMPILE. The suite
  # fails to build, no pin runs, and the old code announced "REDDENED NOTHING:
  # the pins are blind to this mutation" because it tested for the ABSENCE of
  # a FAIL line. That is an invitation to delete a pin that works, on evidence
  # that was never gathered. Absence of a failure is not evidence of survival.
  # ------------------------------------------------------------------------
  VERDICT="INCONCLUSIVE"
  WHY=""
  if [ ! -s "$OUT" ]; then
    VERDICT="INCONCLUSIVE"
    WHY="the suite produced no output at all"
  elif grep -qE '^FAIL  \(' "$OUT"; then
    VERDICT="REDDENED"
    WHY="$(grep -m1 -E '^FAIL  \(' "$OUT")"
  elif grep -qE '^PASS  \(' "$OUT"; then
    VERDICT="SURVIVED"
    WHY="$(grep -m1 -E '^PASS  \(' "$OUT")"
  elif [ "$RUNNER_RC" -ne 0 ]; then
    WHY="the suite exited $RUNNER_RC with no verdict line, so it did not run to completion"
  else
    WHY="the suite exited 0 but printed no PASS ( or FAIL ( line"
  fi

  case "$VERDICT" in
    REDDENED)
      echo "VERDICT: REDDENED  ($WHY)"
      echo "         the pins saw this mutation" ;;
    SURVIVED)
      echo "VERDICT: SURVIVED  ($WHY)"
      echo "         THE PINS ARE BLIND TO THIS MUTATION. That is a finding," 
      echo "         not a success: the suite ran to completion and nothing" 
      echo "         objected to a deliberate defect." ;;
    *)
      echo "VERDICT: INCONCLUSIVE ($WHY)"
      echo "         NOTHING WAS MEASURED. Do not read this as a surviving"
      echo "         mutation and do not delete a pin on it. The most likely"
      echo "         cause is a mutation that does not compile; check the"
      echo "         suite's own output before concluding anything."
      if [ "$RUNNER_RC" -ne 0 ]; then
        echo "         --- last 6 lines of the suite's output ---"
        tail -6 "$OUT" | sed 's/^/         /'
      fi ;;
  esac
  rm -f "$OUT"

  cp "$PRISTINE" "$TARGET"
  echo "restored: $(shasum -a 256 "$TARGET" | cut -c1-16)"
done

restore
echo ""
if [ "$restored_ok" = "1" ]; then
  echo "=== ALL RESTORED, $TARGET matches baseline $BASELINE"
else
  echo "=== RESTORE DID NOT VERIFY. $TARGET IS NOT BASELINE."
fi
echo "=== MUTATEDONE"
