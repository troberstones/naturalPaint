#!/bin/bash
# tools/modal-shots/capture_modals.sh -- photograph every modal dialog.
#
# One PNG per dialog under docs/modal-screenshots/, cropped to the dialog
# itself, for reviewing this application's modals against platform UI
# conventions. Companion prose: docs/modal-screenshots/README.md.
#
# **Not a regression harness.** tools/golden/run_golden.sh is that, and this
# script deliberately does not diff anything or fail on a changed pixel -- its
# output is a contact sheet for a human to look at, regenerated on demand.
# Anything here that should be *pinned* belongs in a golden view instead.
#
# Two pieces of machinery do the work, both added for this:
#
#   * `--open-modal <MenuActionName>` (src/main.cpp) enqueues one menu action
#     on the first frame, through the same queue the native menu bar uses.
#     Every dialog below is opened by exactly one MenuAction, so this is one
#     flag rather than thirty; the names come from `menuActionName()`.
#   * `--screenshot` prints `[screenshot] modal "<title>" rect x y w h` when a
#     popup is open, read off ImGui's own `OpenPopupStack`. These dialogs are
#     `AlwaysAutoResize` and centred, so their rect is a function of their
#     content -- a hardcoded crop box would start cutting them in half the
#     first time a label got a word longer, which is exactly the sort of
#     change a UI review is looking for.
#
# Crop-by-diff against a no-dialog baseline was the other candidate and cannot
# work here: seven of these dialogs live-preview into the canvas, so the region
# that differs from a plain launch is the whole window.
#
# Preference isolation is run_golden.sh's, for run_golden.sh's reasons (read
# its header): the app reads a panel layout, a brush registry, a dab folder, a
# document-preset list and an export-preset file out of ~/Library/Application
# Support/naturalPaint, and each one changes what gets photographed. Every
# capture gets its own empty scratch copies. The document-preset one is not
# incidental here -- it sizes the New Document dialog's own list box.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${NP_BIN:-$ROOT/build/src/naturalPaint}"
TOOL="${NP_GOLDEN_TOOL:-$ROOT/build/src/goldentool}"
OUT="${1:-$ROOT/docs/modal-screenshots}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/np-modal-shots.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# Padding around the dialog's own rect, in framebuffer pixels. Enough to show
# the dialog is a floating window over the application rather than a cropped
# rectangle of widgets, without dragging in enough chrome to dominate the
# frame at review size.
PAD=28

# Settle frames. run_golden.sh's own 90, and for the same reason: an ImGui
# hover/press tint is a time-based lerp that frame-counting alone does not
# guarantee has finished.
FRAMES=90

[ -x "$BIN" ] || { echo "capture_modals.sh: no binary at $BIN -- cmake --build build" >&2; exit 2; }
[ -x "$TOOL" ] || { echo "capture_modals.sh: no goldentool at $TOOL" >&2; exit 2; }
mkdir -p "$OUT"

# name | extra args. `--demo-document` is on every row: these dialogs act on
# the active document, and several of them (Image Size, Canvas Size, Levels'
# histogram, the two Range dialogs) read it to fill their own fields, so a
# session with no document photographs a dialog in a state a user would only
# see by accident.
#
# bash 3.2 (macOS's /bin/bash): parallel indexed arrays, not an associative
# one -- run_golden.sh's own constraint and its own reason.
names=(); args=()
add() { names+=("$1"); args+=("$2"); }

add new-document          "--open-modal NewDocument"
add revert                "--open-modal Revert"
add recover-documents     "--open-modal RecoverDocuments"
add export-as             "--open-modal ExportAs"
add export-states         "--open-modal ExportStates"

add filter-gaussian-blur  "--open-modal GaussianBlur"
add filter-sharpen        "--open-modal Sharpen"
add filter-unsharp-mask   "--open-modal UnsharpMask"
add filter-add-noise      "--open-modal AddNoise"
add filter-emboss         "--open-modal Emboss"
add filter-median         "--open-modal Median"
add filter-motion-blur    "--open-modal MotionBlur"

add image-size            "--open-modal ImageSize"
add canvas-size           "--open-modal CanvasSize"
add numeric-transform     "--open-modal NumericTransform"

add adjust-levels             "--open-modal AdjustLevels"
add adjust-curves             "--open-modal AdjustCurves"
add adjust-exposure           "--open-modal AdjustExposure"
add adjust-channel-mixer      "--open-modal AdjustChannelMixer"
add adjust-brightness-contrast "--open-modal AdjustBrightnessContrast"
add adjust-hue-saturation     "--open-modal AdjustHueSaturation"
add adjust-vibrance           "--open-modal AdjustVibrance"
add adjust-colour-balance     "--open-modal AdjustColorBalance"
add adjust-black-and-white    "--open-modal AdjustBlackAndWhite"
add adjust-photo-filter       "--open-modal AdjustPhotoFilter"
add adjust-posterize          "--open-modal AdjustPosterize"
add adjust-threshold          "--open-modal AdjustThreshold"
add adjust-gradient-map       "--open-modal AdjustGradientMap"

# The three refine dialogs share one `RefineRadiusDialog` body, so all three
# are photographed rather than one standing in for the others -- the shared
# body is exactly what makes a per-dialog label drift invisible.
add select-grow           "--open-modal SelectGrow"
add select-shrink         "--open-modal SelectShrink"
add select-feather        "--open-modal SelectFeather"
add select-colour-range   "--open-modal SelectColourRange"
add select-luminance-range "--open-modal SelectLuminanceRange"

# Not a MenuAction: the LAYERS panel's gear button opens it, and this flag
# predates --open-modal.
add layer-properties      "--open-layer-properties"

# View > Add Guide... used to be the one dialog this script could not capture:
# it was a non-modal `BeginPopup`, and ImGui closes those the moment anything
# else takes focus (`FocusWindow()` -> `ClosePopupsOverWindow()`), so it never
# survived to a captured frame. It is a `beginDialog()` modal now, like the
# rest (docs/modal-screenshots/README.md, S9), and photographs like the rest.
add add-guide             "--open-modal AddGuide"

failed=0
for i in $(seq 0 $((${#names[@]} - 1))); do
  name="${names[$i]}"
  jdir="$WORK/$name"
  mkdir -p "$jdir"
  full="$jdir/full.png"

  # shellcheck disable=SC2086 -- ${args[$i]} is a deliberate word split.
  if ! NP_JOURNAL_DIR="$jdir" \
      NP_PANEL_LAYOUT="$jdir/panel-layout.txt" \
      NP_BRUSH_LIBRARIES="$jdir/brush-libraries.txt" \
      NP_DAB_DIR="$jdir/dabs-root" \
      NP_DOCUMENT_PRESETS="$jdir/document-presets.txt" \
      NP_EXPORT_PRESETS="$jdir/export-presets.json" \
      "$BIN" --demo-document ${args[$i]} --screenshot "$full" "$FRAMES" \
      > "$jdir/stdout.log" 2> "$jdir/stderr.log"; then
    echo "$name: naturalPaint exited nonzero -- $jdir/stderr.log" >&2
    failed=$((failed + 1))
    continue
  fi

  # `[screenshot] wrote <path> (WxH)` and `[screenshot] modal "<t>" rect x y w h`.
  imgWH="$(sed -n 's/.*\[screenshot\] wrote .*(\([0-9]*\)x\([0-9]*\)).*/\1 \2/p' "$jdir/stdout.log")"
  rect="$(sed -n 's/.*\[screenshot\] modal .* rect \(.*\)/\1/p' "$jdir/stdout.log")"
  if [ -z "$rect" ]; then
    # The flag was accepted (the app exited 0) and no popup was open on the
    # captured frame. Said out loud rather than silently emitting the plain
    # window: a dialog that quietly failed to open is precisely what this
    # script would otherwise document as "the dialog looks like the app".
    echo "$name: no modal was open on the captured frame -- see $jdir/stdout.log" >&2
    failed=$((failed + 1))
    continue
  fi
  set -- $rect; mx=$1; my=$2; mw=$3; mh=$4
  set -- $imgWH; imgW=$1; imgH=$2

  cx=$((mx - PAD)); cy=$((my - PAD))
  cw=$((mw + 2 * PAD)); ch=$((mh + 2 * PAD))
  [ "$cx" -lt 0 ] && { cw=$((cw + cx)); cx=0; }
  [ "$cy" -lt 0 ] && { ch=$((ch + cy)); cy=0; }
  [ $((cx + cw)) -gt "$imgW" ] && cw=$((imgW - cx))
  [ $((cy + ch)) -gt "$imgH" ] && ch=$((imgH - cy))

  if ! "$TOOL" crop "$full" "$OUT/$name.png" "$cx" "$cy" "$cw" "$ch" > "$jdir/crop.log" 2>&1; then
    echo "$name: crop failed -- $jdir/crop.log" >&2
    failed=$((failed + 1))
    continue
  fi
  echo "$name  ${mw}x${mh} at ${mx},${my}"
done

echo
if [ "$failed" -gt 0 ]; then
  echo "$failed of ${#names[@]} views failed; $WORK kept" >&2
  trap - EXIT
  exit 1
fi
echo "${#names[@]} dialogs -> $OUT"
