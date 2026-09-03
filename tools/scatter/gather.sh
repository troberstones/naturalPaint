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

if [ $# -lt 1 ]; then
  echo "usage: $0 <track> [<track> ...]" >&2
  exit 2
fi

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
COMMON="$(cd "$(dirname "$(git -C "$HERE" rev-parse --git-common-dir)")" && pwd)"
WT_ROOT="$COMMON/.claude/worktrees"
LOG_ROOT="${SCATTER_LOG_ROOT:-/private/tmp/np-scatter}"
GATHER_LOG="$LOG_ROOT/gather"
mkdir -p "$GATHER_LOG"

if [ -n "$(git -C "$HERE" status --porcelain)" ]; then
  echo "gather: the integration worktree is dirty -- commit or set aside first." >&2
  exit 1
fi

fail() { echo "gather: FAIL -- $*" >&2; exit 1; }

# --- gate 1 + 2: per-track, before anything is merged ----------------------
for TRACK in "$@"; do
  WT="$WT_ROOT/scatter-$TRACK"
  [ -d "$WT" ] || fail "no worktree for track '$TRACK' at $WT"

  if [ -n "$(git -C "$WT" status --porcelain)" ]; then
    fail "track '$TRACK' has uncommitted changes -- its agent may still be running"
  fi

  # Grep the track's own commits, not the working tree: the tree is what the
  # agent is still touching, the commits are what would actually merge.
  RANGE="$(git -C "$HERE" merge-base HEAD "scatter/$TRACK")..scatter/$TRACK"
  if git -C "$WT" diff "$RANGE" -- src third_party tools | grep -n 'SABOTAGE'; then
    fail "track '$TRACK' still contains SABOTAGE markers (shown above)"
  fi
  echo "gather: track '$TRACK' clean and sabotage-free"
done
echo

# --- merge, one at a time, stopping on conflict ----------------------------
for TRACK in "$@"; do
  echo "gather: merging scatter/$TRACK"
  if ! git -C "$HERE" merge --no-ff --no-edit "scatter/$TRACK" > "$GATHER_LOG/merge-$TRACK.log" 2>&1; then
    echo
    cat "$GATHER_LOG/merge-$TRACK.log"
    echo
    git -C "$HERE" diff --name-only --diff-filter=U
    fail "merge of '$TRACK' conflicted -- resolve BY HAND (never union), then re-run for the remaining tracks"
  fi
done
echo

# --- gate 2 again, on the merged result ------------------------------------
# A merge can reintroduce a marker that neither side's diff showed, and this
# is the last point before a build is trusted.
if grep -rn 'SABOTAGE' "$HERE/src" "$HERE/tools" 2>/dev/null | grep -v 'scatter/gather.sh'; then
  fail "merged tree contains SABOTAGE markers (shown above)"
fi
echo "gather: merged tree is sabotage-free"

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
set +e
"$BIN" --selftest > "$GATHER_LOG/selftest.log" 2>&1
RC=$?
set -e
FAILS="$(grep -c '^FAIL' "$GATHER_LOG/selftest.log" || true)"
PASSES="$(grep -c '^ok' "$GATHER_LOG/selftest.log" || true)"
tail -3 "$GATHER_LOG/selftest.log"
[ "$RC" -eq 0 ] || fail "--selftest exited $RC (log: $GATHER_LOG/selftest.log)"
[ "$FAILS" -eq 0 ] || fail "$FAILS FAIL line(s) in --selftest (log: $GATHER_LOG/selftest.log)"

echo
echo "gather: OK -- $# track(s) merged, $PASSES assertions pass, 0 FAIL"
echo "gather: logs in $GATHER_LOG"
