#!/usr/bin/env bash
#
# start.sh -- build whatever is in the tree right now, then launch it.
#
# The default is deliberately "build, then run", because the failure this
# script exists to prevent is running a STALE BINARY: the build fails, the old
# executable is still sitting in build/src/, it launches happily, and you spend
# twenty minutes testing the change you did not just make. So a failed build
# here is fatal and nothing is launched.
#
#   ./start.sh                  build, then launch the app
#   ./start.sh --selftest       build, then run the assertion suite instead
#   ./start.sh -n               launch without building (warns if stale)
#   ./start.sh -c               reconfigure from scratch, then build and launch
#   ./start.sh -- --foo bar     pass --foo bar through to the binary
#
# Any argument this script does not recognise is passed through to the binary,
# so `./start.sh --diag` and `./start.sh some/file.png` work as you'd expect.
#
# stdout and stderr are written to SEPARATE log files under build/logs/ and
# also echoed to your terminal. They are never merged: a crash backtrace
# interleaved into the middle of assertion output is how a real failure gets
# read as noise.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$REPO/build"
BIN="$BUILD/src/naturalPaint"
LOGS="$BUILD/logs"
OIIO="${NP_OIIO_PREFIX:-$HOME/.local/openimageio}"
JOBS="${NP_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

do_build=1
do_clean=0
declare -a passthrough=()

while [ $# -gt 0 ]; do
  case "$1" in
    -n|--no-build) do_build=0 ;;
    -c|--clean)    do_clean=1 ;;
    -h|--help)     sed -n '3,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --)            shift; passthrough+=("$@"); break ;;
    *)             passthrough+=("$1") ;;
  esac
  shift
done

say()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m  %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m  %s\n' "$*" >&2; exit 1; }

mkdir -p "$LOGS"

# --- The submodule trap -----------------------------------------------------
# A fresh clone or a fresh worktree has an EMPTY third_party/mixbox, and every
# run then dies on the missing Mixbox LUT -- with an error that points at the
# LUT, not at the submodule. Checked here because the fix is one command and
# the symptom is not obviously a submodule problem.
#
# The probe is the LUT itself, not some source file in the submodule: it is
# the exact path src/CMakeLists.txt bakes in as NP_MIXBOX_LUT, so it is the
# file whose absence actually stops a run. Probing anything else both misses
# a half-populated checkout and false-positives on a good one.
if [ ! -s "$REPO/third_party/mixbox/shaders/mixbox_lut.png" ]; then
  warn "third_party/mixbox looks empty -- fetching it"
  git -C "$REPO" submodule update --init third_party/mixbox \
    || die "could not init third_party/mixbox; run: git submodule update --init third_party/mixbox"
fi

# --- Configure --------------------------------------------------------------
if [ "$do_clean" -eq 1 ]; then
  say "removing $BUILD"
  rm -rf "$BUILD"
  mkdir -p "$LOGS"
fi

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  [ -d "$OIIO" ] || warn "OpenImageIO not found at $OIIO (override with NP_OIIO_PREFIX=...)"
  say "configuring (RelWithDebInfo)"
  cmake -S "$REPO" -B "$BUILD" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_PREFIX_PATH="$OIIO" \
        >"$LOGS/configure.out" 2>"$LOGS/configure.err" \
    || { tail -30 "$LOGS/configure.err" >&2; die "configure failed -- full log: $LOGS/configure.err"; }
fi

# --- Build ------------------------------------------------------------------
if [ "$do_build" -eq 1 ]; then
  say "building with -j$JOBS"
  cmake --build "$BUILD" -j"$JOBS" >"$LOGS/build.out" 2>"$LOGS/build.err"
  status=$?
  if [ $status -ne 0 ]; then
    grep -E "error:|Error" "$LOGS/build.err" | head -20 >&2
    die "build failed (exit $status) -- NOT launching a stale binary. Full log: $LOGS/build.err"
  fi
  # Warnings are worth surfacing even on success; -Werror means most are
  # already fatal, so anything left here is something the build chose to allow.
  #
  # One line is excluded, deliberately and by exact text: the linker's
  # "ignoring duplicate libraries" note for libSDL3.a / libwgpu_native.a. It
  # fires on EVERY link, it is benign, and reporting it every run would teach
  # you to skip past this line -- at which point a real warning arrives and
  # goes unread. Excluded here rather than silenced at the linker so that it
  # stays visible in build.err, and so this comment is the only place it can
  # be forgotten about.
  warncount=$(grep "warning:" "$LOGS/build.err" 2>/dev/null \
              | grep -vc "ignoring duplicate libraries" || true)
  [ "${warncount:-0}" -gt 0 ] && warn "$warncount build warning(s) -- see $LOGS/build.err"
else
  # --- The staleness check, for the one mode that can hit it ----------------
  # Only reachable with -n. Compares the binary's mtime against the newest
  # file under src/: if any source is newer, what is about to launch is not
  # what is in the tree.
  if [ -x "$BIN" ]; then
    newest=$(find "$REPO/src" -type f \( -name '*.cpp' -o -name '*.hpp' \) -newer "$BIN" -print -quit 2>/dev/null)
    [ -n "$newest" ] && warn "STALE: $(basename "$newest") is newer than the binary -- you are testing an older build"
  fi
fi

[ -x "$BIN" ] || die "no binary at $BIN -- run without -n to build it"

# --- Launch -----------------------------------------------------------------
stamp=$(date +%Y%m%d-%H%M%S)
OUT="$LOGS/run-$stamp.out"
ERR="$LOGS/run-$stamp.err"

say "launching $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo '?')$( \
    [ -n "$(git -C "$REPO" status --porcelain 2>/dev/null)" ] && echo '+dirty')"
[ ${#passthrough[@]} -gt 0 ] && say "args: ${passthrough[*]}"

# Separate streams, both mirrored to the terminal. `2> >(... >&2)` keeps stderr
# on stderr rather than folding it into stdout.
"$BIN" ${passthrough[@]+"${passthrough[@]}"} > >(tee "$OUT") 2> >(tee "$ERR" >&2)
status=$?
wait 2>/dev/null

# --- Report -----------------------------------------------------------------
# --selftest earns a summary, because "exit 0" and "0 FAIL" are not the same
# claim and it is the difference that matters.
if printf '%s\n' ${passthrough[@]+"${passthrough[@]}"} | grep -qx -- '--selftest'; then
  passes=$(grep -c ' pass$' "$OUT" 2>/dev/null || true)
  reds=$(grep -c ' FAIL$' "$OUT" 2>/dev/null || true)
  sections=$(grep -c '^\[selftest\].* FAIL' "$OUT" 2>/dev/null || true)
  say "selftest: $passes pass, $reds failing assertions, $sections failing sections, exit $status"
fi

say "stdout: $OUT"
say "stderr: $ERR"
exit $status
