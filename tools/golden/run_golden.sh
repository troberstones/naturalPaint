#!/bin/bash
# tools/golden/run_golden.sh -- the golden-image regression harness.
#
# app/Screenshot.hpp makes the app photograph its own swapchain (no macOS
# screen-recording permission, exact rendered pixels -- see that header for
# why). This script is the missing other half: it drives the app through a
# handful of known, scripted UI states via its existing --demo-document /
# --ui-layer-demo / --pigment-stroke-demo / --marquee-demo CLI flags, crops
# each capture down
# to one small region, and compares it against a reference image committed
# under tests/golden/ with src/tools/GoldenTool.cpp.
#
# Written against bash 3.2 (macOS's /bin/bash) deliberately -- no namerefs,
# no associative arrays -- so views are parallel indexed arrays below rather
# than one table keyed by name.
#
# NOT run by --selftest, and not wired into any CI (this project has none).
# It needs a real window and a real GPU adapter, which --selftest's headless
# sections deliberately do not require. Run it explicitly, in one command:
#
#   tools/golden/run_golden.sh check             # compare against tests/golden/*.png
#   tools/golden/run_golden.sh update             # regenerate tests/golden/*.png from the current build
#   tools/golden/run_golden.sh measure [N=10]     # run-to-run reproducibility: N launches per view,
#                                                  # each diffed against the first, noise distribution printed
#
# Every view is captured with its own empty $NP_JOURNAL_DIR (see
# app/Journal.cpp). Without this, a prior run's crash-recovery scratch
# directory makes the app pop a "Recover Documents" modal over the whole
# window on the next launch -- non-deterministic, and dependent on what this
# same script did five minutes ago rather than on the code under test.
# Discovered by hitting it directly while picking these views.
#
# Reference images are kept tightly cropped, not full-window, both for
# repository size and because a smaller region is less likely to contain an
# incidental unstable pixel far from what the view is actually proving.
#
# Every view is captured at 90 settle frames (--screenshot's [frames]
# argument), not the app's own default of 30. Measured cause: --ui-layer-demo
# leaves the synthetic pointer sitting over a LAYERS-panel row, and that
# row's hover/press tint is a continuous, time-based ImGui lerp -- not
# something frame-counting alone guarantees has finished. At 30 frames,
# N=25 launches of the `layers` view against launch 1 produced 4 non-zero
# comparisons (max channel diff up to 70, up to 14 px of a 121 600 px crop).
# The other two views, whose demo scripts leave the pointer nowhere
# interactive, were exact-zero across every trial at 30 frames already.
# Raising settle to 90 frames cut `layers`' flake rate roughly 3x (from 4/24
# to 1/19 in a follow-up N=20 batch) and its worst observed max channel diff
# to 25 -- consistent with an exponential-decay lerp whose probability of a
# last-bit rounding flip shrinks but never guarantees zero in finite frames.
#
# So thresholds below are per-view and measured, not guessed:
#   toolbar, canvas: 0 (exact byte equality). Zero non-zero comparisons in
#     every trial run at this repo's HEAD -- 24 at 30 frames plus 9 more at
#     90 frames each, 66 total pairwise comparisons, 0 non-zero. A raw
#     byte-for-byte compare is enough here, so goldentool's channel-aware
#     diff is used at threshold 0 rather than reaching for a perceptual
#     comparator this measurement gives no reason to need.
#   layers: magnitude 96, changed-px 64. The magnitude figure is derived
#     from the 90-frame measurement's worst observed max channel diff (25),
#     roughly 4x that as headroom against a residual animation-timing tail
#     this measurement cannot fully rule out.
#
#     The changed-px figure exists because that magnitude floor, on its own,
#     was measurably blind. Lightening `kChromeBase` by 40 per channel --
#     a plainly visible regression -- moved 92 516 of this view's 121 600
#     pixels (76%) and the view reported PASS, because 40 <= 96. The noise
#     this floor was bought to tolerate is at most 14 changed px, so a
#     budget of 64 (~4.5x) keeps the tolerance and closes the hole: the
#     diffuse shift exceeds it by a factor of 1 400.
#
#     Both criteria must hold. Noise is few-pixels-at-moderate-magnitude; a
#     regression is either many pixels or one pixel moved far, and those are
#     different axes.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${NP_GOLDEN_BUILD_DIR:-$REPO_ROOT/build}"
BIN="${NP_GOLDEN_BIN:-$BUILD_DIR/src/naturalPaint}"
TOOL="${NP_GOLDEN_TOOL:-$BUILD_DIR/src/goldentool}"
REF_DIR="$REPO_ROOT/tests/golden"
WORK_DIR="${NP_GOLDEN_WORK_DIR:-$REPO_ROOT/tests/golden/.work}"

if [ ! -x "$BIN" ]; then
  echo "run_golden.sh: no naturalPaint binary at $BIN -- build first" >&2
  exit 2
fi
if [ ! -x "$TOOL" ]; then
  echo "run_golden.sh: no goldentool binary at $TOOL -- build first" >&2
  exit 2
fi

mode="${1:-check}"
measure_n="${2:-10}"

# --- view table (three parallel indexed arrays; see header note on why) ---
#
#   toolbar  -- tab strip + the Brush tool's SIZE/HARD/LOAD/WET parameter
#     sliders under --demo-document. Chrome: exercises text, sliders, tab
#     strip glyphs.
#   layers  -- three LAYERS panel rows (Pigment/Adjustment/RGB kind glyphs,
#     names, checkboxes) under --demo-document --ui-layer-demo, which builds
#     a 5-layer stack exercising every layer kind's icon. Layer rows, named
#     explicitly in the task this harness answers.
#   canvas  -- the yellow/blue pigment strokes crossing to green under
#     --pigment-stroke-demo (PLAN.md's own Phase 5 verify sentence: latent
#     Mix of blue over yellow gives green). Canvas: real simulated paint
#     content, not just a static composited rectangle.
#   tools  -- the whole 2-column tool palette under --demo-document
#     --marquee-demo, with Rectangle Marquee selected so both the glyph and
#     the selected-button treatment are in frame. Every tool's icon is drawn
#     by hand into an ImDrawList (ui/MacPaintUI's drawToolIcon), so a tool
#     added without a glyph renders as a bare square and nothing else in this
#     project notices -- which is exactly what happened when Tool::Marquee
#     landed. This view is the thing that would have noticed.
#
#     **The marching ants are deliberately NOT a golden view**, though
#     --marquee-demo draws them a few hundred pixels to the right of this
#     crop. Their dash phase advances on `ImGui::GetTime()` -- wall clock, so
#     the ants are at a different phase at frame 90 on every launch, by
#     design (they crawl at the same speed at 30 fps and 120). A tolerance
#     wide enough to admit an arbitrary phase would admit almost any change
#     to that boundary, so it would be a test in name only. The ants are
#     verified by screenshot instead, and the palette -- which is static -- is
#     what gets locked at byte equality.
view_names=(toolbar layers canvas tools)
view_args=("--demo-document" "--demo-document --ui-layer-demo" "--pigment-stroke-demo" "--demo-document --marquee-demo")
view_crop_x=(0    1916 1024 8)
view_crop_y=(77   1075 973  250)
view_crop_w=(1400 640  384  216)
view_crop_h=(175  190  256  450)
view_frames=(90 90 90 90)
view_threshold=(0 96 0 0)
# The second criterion: how many pixels may differ at all, whatever their
# magnitude. See goldentool's runDiff() for why one threshold is not enough.
# toolbar/canvas are 0 because their magnitude threshold is 0 -- there the
# two criteria say the same thing. `layers` is 64: the 90-frame measurement's
# worst observed count was 14 changed px of 121 600, so this is ~4.5x that,
# and still 1400x below the 92 516 px that the diffuse-shift test moved.
# `tools` is 0 for the same reason as toolbar/canvas: it is static chrome
# with no animation and no simulation behind it, so anything but byte
# equality is a real change. Confirmed by `measure`, not assumed -- see the
# note in cmd_measure on what that mode is for.
view_max_changed_px=(0 64 0 0)

# Captures view index $1 (into the app's full-window screenshot, then
# cropped) to path $2, using scratch journal dir $3. Echoes nothing on
# success; returns nonzero and leaves logs in $WORK_DIR on failure.
run_view_capture() {
  local idx="$1" outPng="$2" jdir="$3"
  local name="${view_names[$idx]}"
  local fullPng="$WORK_DIR/${name}.full.png"
  mkdir -p "$jdir"
  if ! NP_JOURNAL_DIR="$jdir" "$BIN" ${view_args[$idx]} --screenshot "$fullPng" "${view_frames[$idx]}" \
      > "$WORK_DIR/${name}.stdout.log" 2> "$WORK_DIR/${name}.stderr.log"; then
    echo "run_golden.sh: $name: naturalPaint exited nonzero -- see $WORK_DIR/${name}.stderr.log" >&2
    return 1
  fi
  "$TOOL" crop "$fullPng" "$outPng" "${view_crop_x[$idx]}" "${view_crop_y[$idx]}" \
      "${view_crop_w[$idx]}" "${view_crop_h[$idx]}" > "$WORK_DIR/${name}.crop.log" 2>&1
}

cmd_check() {
  mkdir -p "$WORK_DIR"
  local failed=0
  local idx=0
  for name in "${view_names[@]}"; do
    local jdir="$WORK_DIR/${name}_journal"
    local actual="$WORK_DIR/${name}.actual.png"
    rm -rf "$jdir"
    if ! run_view_capture "$idx" "$actual" "$jdir"; then
      failed=1
      idx=$((idx + 1))
      continue
    fi
    local ref="$REF_DIR/${name}.png"
    if [ ! -f "$ref" ]; then
      echo "[$name] FAIL -- no reference image at $ref (run 'update' to create it)"
      failed=1
      idx=$((idx + 1))
      continue
    fi
    local diffOut="$REF_DIR/${name}.diff.png"
    rm -f "$diffOut" "$REF_DIR/${name}.actual.png"
    if "$TOOL" diff "$actual" "$ref" "$diffOut" "${view_threshold[$idx]}" \
        "${view_max_changed_px[$idx]}" > "$WORK_DIR/${name}.diff.log" 2>&1; then
      echo "[$name] PASS"
    else
      cat "$WORK_DIR/${name}.diff.log"
      cp "$actual" "$REF_DIR/${name}.actual.png"
      echo "[$name] FAIL -- actual: $REF_DIR/${name}.actual.png  reference: $ref  diff: $diffOut"
      failed=1
    fi
    idx=$((idx + 1))
  done
  if [ $failed -ne 0 ]; then
    echo "run_golden.sh: FAIL"
    return 1
  fi
  echo "run_golden.sh: PASS (${#view_names[@]} views)"
  return 0
}

cmd_update() {
  mkdir -p "$WORK_DIR" "$REF_DIR"
  local idx=0
  for name in "${view_names[@]}"; do
    local jdir="$WORK_DIR/${name}_journal"
    local out="$REF_DIR/${name}.png"
    rm -rf "$jdir"
    if ! run_view_capture "$idx" "$out" "$jdir"; then
      echo "[$name] update FAILED"
      idx=$((idx + 1))
      continue
    fi
    local bytes
    bytes=$(stat -f%z "$out" 2>/dev/null || stat -c%s "$out")
    echo "[$name] wrote $out ($bytes bytes)"
    idx=$((idx + 1))
  done
}

cmd_measure() {
  mkdir -p "$WORK_DIR"
  echo "run_golden.sh measure: N=$measure_n launches per view, each diffed against launch 1"
  local idx=0
  for name in "${view_names[@]}"; do
    echo "=== $name ==="
    local first=""
    for i in $(seq 1 "$measure_n"); do
      local jdir="$WORK_DIR/${name}_measure_${i}_journal"
      local out="$WORK_DIR/${name}_measure_${i}.png"
      rm -rf "$jdir"
      run_view_capture "$idx" "$out" "$jdir" > /dev/null
      if [ "$i" -eq 1 ]; then
        first="$out"
      else
        echo -n "  run1 vs run$i: "
        # Both thresholds wide open: measure is reporting the noise
        # distribution, not judging it. The verdict line is discarded; only
        # the statistics line is read.
        "$TOOL" diff "$out" "$first" "$WORK_DIR/${name}_measure_diff_${i}.png" 255 999999999 \
          | grep "goldentool diff:" | head -1
      fi
    done
    idx=$((idx + 1))
  done
}

case "$mode" in
  check) cmd_check ;;
  update) cmd_update ;;
  measure) cmd_measure ;;
  *) echo "usage: $0 [check|update|measure [N]]" >&2; exit 2 ;;
esac
