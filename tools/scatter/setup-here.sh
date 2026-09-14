#!/usr/bin/env bash
# tools/scatter/setup-here.sh -- turn the worktree you are standing in into a
# scatter track: pinned base, Mixbox, fast configure, one pre-built target.
#
#   bash tools/scatter/setup-here.sh <track> <base-sha> [<cmake-target>]
#
# scatter.sh builds worktrees FOR agents. That stopped working on 2026-09-11:
# the harness only lets a session's subagents write inside a worktree the
# harness itself created for them (`isolation: "worktree"`), so a worktree the
# dispatcher pre-built is read-only to its agent. The agent now starts in a
# harness worktree -- based on whatever the harness chose, which has been a
# stale commit here before -- and runs this script first, in place. It carries
# the same guards as scatter.sh:
#
#  * the base is an explicit SHA, checked out onto branch scatter/<track> and
#    verified, so a wrong starting commit cannot survive the first command;
#  * third_party/mixbox is initialised, or every run dies on the Mixbox LUT;
#  * CMake borrows the primary checkout's _deps (~32 s instead of ~560 s);
#  * logs go to build/scatter-logs/ -- private to this worktree, and ignored
#    by git because build/ is.
#
# Needs a clean tree: it refuses rather than check out over someone's work.
set -euo pipefail

if [ $# -lt 2 ]; then
  echo "usage: $0 <track> <base-sha> [<cmake-target>]" >&2
  exit 2
fi
TRACK="$1"
BASE="$2"
TARGET="${3:-}"

WT="$(git rev-parse --show-toplevel)"
COMMON_ROOT="$(cd "$(dirname "$(git -C "$WT" rev-parse --git-common-dir)")" && pwd)"
DEPS="$COMMON_ROOT/build/_deps"
LOGS="$WT/build/scatter-logs"

if [ -n "$(git -C "$WT" status --porcelain --ignore-submodules=all)" ]; then
  echo "setup-here: $WT has uncommitted changes -- refusing to check out over them." >&2
  exit 1
fi
git -C "$WT" rev-parse --verify --quiet "${BASE}^{commit}" >/dev/null \
  || { echo "setup-here: '$BASE' is not a commit in this repository." >&2; exit 1; }

git -C "$WT" checkout -q -B "scatter/$TRACK" "$BASE"
HEAD_NOW="$(git -C "$WT" rev-parse HEAD)"
BASE_FULL="$(git -C "$WT" rev-parse "${BASE}^{commit}")"
[ "$HEAD_NOW" = "$BASE_FULL" ] || { echo "setup-here: HEAD is $HEAD_NOW, expected $BASE_FULL" >&2; exit 1; }

git -C "$WT" submodule update --init --quiet third_party/mixbox
mkdir -p "$LOGS"

for d in "$DEPS/imgui-src" "$DEPS/sdl3-src"; do
  [ -d "$d" ] || { echo "setup-here: expected populated dependency at $d" >&2; exit 1; }
done
cmake -S "$WT" -B "$WT/build" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$HOME/.local/openimageio" \
  -DFETCHCONTENT_SOURCE_DIR_SDL3="$DEPS/sdl3-src" \
  -DFETCHCONTENT_SOURCE_DIR_IMGUI="$DEPS/imgui-src" \
  > "$LOGS/configure.log" 2>&1 \
  || { echo "setup-here: configure failed -- see $LOGS/configure.log" >&2; tail -20 "$LOGS/configure.log" >&2; exit 1; }

if [ -n "$TARGET" ]; then
  cmake --build "$WT/build" -j6 --target "$TARGET" > "$LOGS/prebuild.log" 2>&1 \
    || { echo "setup-here: pre-build of $TARGET failed -- see $LOGS/prebuild.log" >&2; tail -20 "$LOGS/prebuild.log" >&2; exit 1; }
fi

echo "setup-here: track $TRACK ready"
echo "  worktree : $WT"
echo "  branch   : scatter/$TRACK at $(git -C "$WT" rev-parse --short HEAD)"
echo "  logs     : $LOGS"
[ -n "$TARGET" ] && echo "  built    : $TARGET"
exit 0
