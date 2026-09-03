#!/usr/bin/env bash
# tools/scatter/gather.sh -- merge finished agent tracks and prove the result.
#
#   ./tools/scatter/gather.sh <track> [<track> ...]
#
# Run from the integration worktree, on the branch the tracks should land on.
# Tracks are merged in the order given; the merge STOPS on the first conflict
# so a human resolves it. Line-level union resolution is banned here: it has
# already eaten a `break;`, a `|| tonal` and left a dead unconditional return
# in this repository, and every one of those merged clean and compiled.
#
# The gate order matters and is not arbitrary:
#
#   1. Each track must be COMMITTED. A dirty worktree means the agent is
#      probably still running, and reading a tree mid-write is how you review
#      a patch that no longer exists.
#   2. `SABOTAGE` is grepped per track BEFORE merging. An agent killed
#      mid-verification leaves a live sabotage in production source; it does
#      not announce itself, and the suite may well still be green because the
#      sabotage is precisely the thing the suite was about to catch.
#   3. Build and test happen HERE, after the merge, from a clean rebuild. No
#      agent's report of a green build is taken as evidence: a build that
#      swallowed an error still exits 0, and a stale binary reruns the old
#      suite happily. This script rebuilds and reruns itself.
#
# Exit status is 0 only if every gate passed.
set -euo pipefail

VERIFY_ONLY=0
if [ "${1:-}" = "--verify-only" ]; then
  VERIFY_ONLY=1
  shift
fi

if [ "$VERIFY_ONLY" -eq 0 ] && [ $# -lt 1 ]; then
  echo "usage: $0 <track> [<track> ...]" >&2
  echo "       $0 --verify-only          (skip merging; run the gates on HEAD)" >&2
  exit 2
fi

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
COMMON="$(cd "$(dirname "$(git -C "$HERE" rev-parse --git-common-dir)")" && pwd)"
WT_ROOT="$COMMON/.claude/worktrees"
LOG_ROOT="${SCATTER_LOG_ROOT:-/private/tmp/np-scatter}"
GATHER_LOG="$LOG_ROOT/gather"
mkdir -p "$GATHER_LOG"

# Where this gather started. Every "did the merge introduce X?" question below
# is asked against this, never against the whole tree -- see the SABOTAGE gate.
# In merge mode this is where the gather started, so the gates below ask "did
# THIS gather introduce it?". In --verify-only mode HEAD is usually the merge
# commit a human just resolved, so the honest comparison is its first parent.
if [ "$VERIFY_ONLY" -eq 1 ]; then
  BASE_HEAD="$(git -C "$HERE" rev-parse 'HEAD^1' 2>/dev/null || git -C "$HERE" rev-parse HEAD)"
else
  BASE_HEAD="$(git -C "$HERE" rev-parse HEAD)"
fi

if [ -n "$(git -C "$HERE" status --porcelain)" ]; then
  echo "gather: the integration worktree is dirty -- commit or set aside first." >&2
  exit 1
fi

fail() { echo "gather: FAIL -- $*" >&2; exit 1; }

# --- gate 1 + 2: per-track, before anything is merged ----------------------
# Skipped entirely by --verify-only, whose whole point is that the merging has
# already happened by hand.
for TRACK in $([ "$VERIFY_ONLY" -eq 1 ] || printf '%s ' "$@"); do
  WT="$WT_ROOT/scatter-$TRACK"
  [ -d "$WT" ] || fail "no worktree for track '$TRACK' at $WT"

  if [ -n "$(git -C "$WT" status --porcelain)" ]; then
    fail "track '$TRACK' has uncommitted changes -- its agent may still be running"
  fi

  # Grep the track's own commits, not the working tree: the tree is what the
  # agent is still touching, the commits are what would actually merge.
  RANGE="$(git -C "$HERE" merge-base HEAD "scatter/$TRACK")..scatter/$TRACK"
  # `^+` matters: a plain grep over the diff also matches CONTEXT lines, and
  # this tree legitimately uses the word SABOTAGE in selftest comments
  # recording sabotages that were run and reverted. Only lines the track ADDS
  # are the track's doing.
  if git -C "$WT" diff "$RANGE" -- src third_party | grep -n '^+.*SABOTAGE'; then
    fail "track '$TRACK' ADDS lines containing SABOTAGE (shown above)"
  fi
  echo "gather: track '$TRACK' clean and sabotage-free"
done
echo

# --- merge, one at a time, stopping on conflict ----------------------------
for TRACK in $([ "$VERIFY_ONLY" -eq 1 ] || printf '%s ' "$@"); do
  echo "gather: merging scatter/$TRACK"
  if ! git -C "$HERE" merge --no-ff --no-edit "scatter/$TRACK" > "$GATHER_LOG/merge-$TRACK.log" 2>&1; then
    echo
    cat "$GATHER_LOG/merge-$TRACK.log"
    echo
    git -C "$HERE" diff --name-only --diff-filter=U
    fail "merge of '$TRACK' conflicted -- resolve BY HAND (never union), commit, then re-run for the REMAINING tracks; if none remain, run '$0 --verify-only' so the build and suite gates still run"
  fi
done
echo

# --- gate 2 again, on the merged result ------------------------------------
# A merge can reintroduce a marker that neither side's diff showed, and this
# is the last point before a build is trusted.
#
# Scoped to what this gather actually changed, NOT to the whole tree. Grepping
# all of src/ was the first version and it was useless: this tree has a dozen
# legitimate SABOTAGE mentions in selftest comments recording sabotages that
# were run and reverted (app/selftest/TransformSession.cpp's "SABOTAGE PROOF"
# sections, and others). A gate that fires every time is a gate you learn to
# ignore, which is worse than no gate at all.
#
# tools/ is excluded for a second, sillier reason found the same way: this
# script's own source names the marker it greps for, so scanning tools/ made
# every edit to this file fail its own gate.
if git -C "$HERE" diff "$BASE_HEAD"..HEAD -- src third_party \
     | grep -n '^+.*SABOTAGE'; then
  fail "this gather ADDS lines containing SABOTAGE (shown above)"
fi
echo "gather: nothing this gather added contains SABOTAGE"

# --- gate 3: rebuild here, from scratch of the changed objects -------------
echo "gather: rebuilding"
if ! cmake --build "$HERE/build" -j8 > "$GATHER_LOG/build.log" 2>&1; then
  tail -40 "$GATHER_LOG/build.log" >&2
  fail "build failed -- full log at $GATHER_LOG/build.log"
fi
# A build can print an error and still exit 0 if a custom command swallowed
# it, and the binary then predates the merge. Check both facts directly.
BIN="$HERE/build/src/naturalPaint"
[ -x "$BIN" ] || fail "no binary at $BIN after a build that exited 0"
if grep -E '(^| )error:' "$GATHER_LOG/build.log"; then
  fail "build log contains errors despite exiting 0"
fi
WARN="$(grep -c 'warning:' "$GATHER_LOG/build.log" || true)"
echo "gather: build clean ($WARN warning lines in log)"

# --- gate 3b: the full suite, run here -------------------------------------
echo "gather: running --selftest"
# stdout and stderr go to SEPARATE files, deliberately. Merging them with
# `2>&1` lets a stderr write land mid-line in stdout: one gather here produced
#     sniff: a null pointer is Unknown ... it paslibpng error: IDAT: ...
# which split the word "pass" and undercounted the suite by one. The counts
# below are only trustworthy if nothing else is writing to the stream.
set +e
"$BIN" --selftest > "$GATHER_LOG/selftest.log" 2> "$GATHER_LOG/selftest.stderr.log"
RC=$?
set -e
FAILS="$(grep -c '^FAIL' "$GATHER_LOG/selftest.log" || true)"
PASSES="$(grep -c '^ok' "$GATHER_LOG/selftest.log" || true)"
tail -3 "$GATHER_LOG/selftest.log"
[ "$RC" -eq 0 ] || fail "--selftest exited $RC (log: $GATHER_LOG/selftest.log)"
[ "$FAILS" -eq 0 ] || fail "$FAILS FAIL line(s) in --selftest (log: $GATHER_LOG/selftest.log)"

echo
if [ "$VERIFY_ONLY" -eq 1 ]; then
  echo "gather: OK -- verified HEAD, $PASSES assertions pass, 0 FAIL"
else
  echo "gather: OK -- $# track(s) merged, $PASSES assertions pass, 0 FAIL"
fi
echo "gather: logs in $GATHER_LOG"
