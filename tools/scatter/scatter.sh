#!/usr/bin/env bash
# tools/scatter/scatter.sh -- set up isolated worktrees for parallel agent tracks.
#
#   ./tools/scatter/scatter.sh <base-sha> <track> [<track> ...]
#
# Every hazard this script guards against has bitten this project before, and
# each guard is here because of a specific incident rather than out of caution:
#
#  * **The base trap.** Letting the agent tool create its own worktree can hand
#    it a STALE base -- a tree that is not the commit you think you are
#    branching from. So the worktree is built here, from an explicit SHA, and
#    the SHA is echoed for the brief to quote.
#  * **The mixbox submodule.** A fresh worktree has an EMPTY
#    third_party/mixbox, and every run then dies on the Mixbox LUT. `git
#    worktree add` does not populate submodules.
#  * **The 560-second configure.** Without FETCHCONTENT_SOURCE_DIR_*, CMake
#    re-downloads and re-extracts SDL3 and Dear ImGui per worktree. Pointing
#    both at the primary checkout's already-populated _deps takes a fresh
#    configure from ~560s to ~32s.
#  * **The shared /tmp collision.** Parallel agents given a brief with
#    hardcoded log paths clobber each other's build output, and the symptom is
#    one track "passing" against another track's binary. Each track gets a
#    private log directory, printed here.
#
# Prints a per-track summary the dispatching brief should quote verbatim.
set -euo pipefail

if [ $# -lt 2 ]; then
  echo "usage: $0 <base-sha> <track> [<track> ...]" >&2
  exit 2
fi

BASE_SHA="$1"; shift
PRIMARY="$(git -C "$(dirname "$0")/../.." rev-parse --show-toplevel)"
# The primary checkout owns the populated _deps; a worktree borrows them.
DEPS="$(git -C "$PRIMARY" rev-parse --git-common-dir)"
DEPS="$(cd "$(dirname "$DEPS")" && pwd)/build/_deps"
WT_ROOT="$(cd "$(dirname "$DEPS")" && pwd)/.claude/worktrees"
LOG_ROOT="${SCATTER_LOG_ROOT:-/private/tmp/np-scatter}"

if ! git -C "$PRIMARY" rev-parse --verify --quiet "${BASE_SHA}^{commit}" >/dev/null; then
  echo "scatter: '$BASE_SHA' is not a commit in this repository." >&2
  exit 1
fi
BASE_FULL="$(git -C "$PRIMARY" rev-parse "${BASE_SHA}^{commit}")"

for d in "$DEPS/imgui-src" "$DEPS/sdl3-src"; do
  [ -d "$d" ] || { echo "scatter: expected populated dependency at $d -- configure the primary build first." >&2; exit 1; }
done

echo "scatter: base $BASE_FULL"
echo

for TRACK in "$@"; do
  WT="$WT_ROOT/scatter-$TRACK"
  BRANCH="scatter/$TRACK"
  LOGS="$LOG_ROOT/$TRACK"

  if [ -e "$WT" ]; then
    echo "scatter: $WT already exists -- remove it first (git worktree remove)." >&2
    exit 1
  fi

  git -C "$PRIMARY" worktree add -q -b "$BRANCH" "$WT" "$BASE_FULL"
  # Without this every run dies on the Mixbox LUT.
  git -C "$WT" submodule update --init --quiet third_party/mixbox
  mkdir -p "$LOGS"

  cmake -S "$WT" -B "$WT/build" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="$HOME/.local/openimageio" \
    -DFETCHCONTENT_SOURCE_DIR_SDL3="$DEPS/sdl3-src" \
    -DFETCHCONTENT_SOURCE_DIR_IMGUI="$DEPS/imgui-src" \
    > "$LOGS/configure.log" 2>&1 \
    || { echo "scatter: configure failed for $TRACK -- see $LOGS/configure.log" >&2; tail -20 "$LOGS/configure.log" >&2; exit 1; }

  echo "track $TRACK"
  echo "  worktree : $WT"
  echo "  branch   : $BRANCH"
  echo "  logs     : $LOGS   (private -- never write build output outside it)"
  echo "  build    : cmake --build $WT/build -j8"
  echo "  test     : $WT/build/src/naturalPaint --selftest"
  echo
done

echo "scatter: $# track(s) ready at base $BASE_FULL"
