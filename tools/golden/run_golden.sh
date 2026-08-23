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
#   toolbar, tools: 0 (exact byte equality). Static chrome, no animation and
#     no simulation behind either one, and zero non-zero comparisons across
#     every trial run measured against them (`toolbar`: 24 at 30 frames plus
#     9 more at 90 frames each at earlier crops, 66 total pairwise
#     comparisons, 0 non-zero -- unaffected by this revision, see below).
#
#     `tools`' crop has moved three times now, each time re-measured at its
#     new geometry rather than assumed to inherit the old crop's zero:
#     kToolPaletteW=44 (85x270, the revision that turned out to clip every
#     icon in half -- see ui/AtelierLayout.hpp's kToolPaletteW comment), the
#     corrected kToolPaletteW=64 (130x270, wide enough to clear the
#     palette's right edge plus its then-permanent scrollbar), and now this
#     revision's 90x270 at kToolPaletteW=44 again -- a *different* 44 than
#     the first one, because the user's own correction ("make the toolbar
#     fit without scrolling ... the buttons are too large") replaced the
#     fixed 36px cell with one computed per frame
#     (ui/AtelierLayout.hpp's atelierToolCellSize()) and removed the
#     scrollbar the 130 was partly sized around, so kToolPaletteW's
#     arithmetic changed even though its numeric value happens to
#     coincide with the buggy revision's. 90, not 44 exactly, for the same
#     "clear the right edge" reasoning as before -- a few px of the
#     canvas's own left edge past the palette's rule, so a regression that
#     widened the palette without updating this crop would still be inside
#     frame rather than silently cropped away. Measured across 5 separately-
#     launched captures (not `measure`'s own loop -- run by hand, each
#     launch spaced a second apart to rule out the load-sensitive artifact
#     `canvas` below ran into) against the current 90x270 (24 300 px)
#     region: 0 mismatched px, max channel diff 0, every time -- still
#     exact-zero. Threshold stays 0 on both criteria.
#   canvas: magnitude 64, changed-px 2624 -- no longer 0. `canvas`'s own
#     reference PNG is untouched (still the same bytes as
#     sidequest/lucide-toolbox's base commit 72fd411), but its crop *x* moved
#     again when kToolPaletteW returned to 44 (904, re-derived by bisection
#     the same way the two earlier corrections were -- x=903 and x=905 both
#     land at ~41% mismatched, x=904 alone drops to a small residual, the
#     same sharp single-integer minimum the 64px correction found at 944).
#     Unlike that earlier round trip, though, this one measured a small,
#     *stable* residual at its minimum rather than exact zero: 656 of 98 304
#     px (0.67%), max channel diff 16, reproduced identically across 6
#     separately-launched captures once each launch was given a couple of
#     seconds' clearance (back-to-back launches under a second apart showed
#     the same 656-or-0 alternate unpredictably -- a load artifact of rapid
#     relaunching, not of the geometry, and not what any real invocation of
#     this script does). The residual itself does not move with load, only
#     the false zeros do, which is what makes it a real property of the
#     x=904 crop rather than noise: this machine's actual framebuffer scale
#     is not a clean 2x of the requested window size (measured directly:
#     2560x1580 physical for a requested 1480x940 logical, i.e. dpr
#     x1.7297/y1.6809, not x2/y2), so an integer *logical* shift in
#     `kToolPaletteW` does not land the crop on the same sub-physical-pixel
#     phase the strokes were antialiased at originally -- a handful of
#     stroke-edge pixels resample slightly differently as a result. 64
#     (4x the measured max channel diff of 16) and 2624 (4x the measured
#     656 changed px) follow `layers`' own precedent below for how much
#     headroom a measured-not-guessed floor gets.
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
#   tools  -- the top of the single-column tool palette under --demo-document
#     --marquee-demo, with Rectangle Marquee selected so both the glyph and
#     the selected-button treatment are in frame (crop covers Move, Marquee,
#     Lasso and part of Polygon Lasso -- sidequest/lucide-toolbox redrew the
#     palette from a 2-wide grid to this single column; see docs/ui.md
#     section 2). Icons are merged Lucide glyphs now
#     (ui/Fonts.cpp's installToolIconFont()), not hand-drawn ImDrawList
#     vectors, but the same lesson the old comment named still holds: a tool
#     whose icon fails to merge/draw renders as a bare square (or, as a real
#     bug during this rebuild briefly did, falls back to the wrong drawing
#     entirely -- see ui/Fonts.cpp's installToolIconFont() comment on
#     `config.DstFont`) and nothing else in this project notices. This view
#     is what would have caught it.
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
# `toolbar`'s height and `canvas`'s x have each moved twice now -- **their
# reference PNGs have not**. sidequest/lucide-toolbox's single-column
# palette first narrowed `kToolPaletteW` from 104 to 64 (docs/ui.md section
# 2 -- 64, not the 44 an earlier revision of this branch shipped: that
# number missed that `ImGuiStyle::WindowPadding` is 8px *per side* and left
# no room at all for the tool grid's then-permanently-visible scrollbar, and
# rendered every icon clipped in half; see ui/AtelierLayout.hpp's
# kToolPaletteW for the full account), which moved `ui/AtelierLayout.cpp`'s
# `canvasX` (= palette width + one rule) 40 logical px to the left -- 80
# physical px (measured: `goldentool diff` against the untouched reference
# was exact-zero at an 80px x-shift and at no other integer offset).
#
# It then moved a second time in the *other* direction: the user's own
# correction ("make the toolbar fit without scrolling ... the buttons are
# too large") replaced the fixed 36px cell with one computed per frame
# (ui/AtelierLayout.hpp's atelierToolCellSize()) and dropped the scrollbar
# entirely, which let `kToolPaletteW` shrink back to 44 -- the same numeral
# the clipping-bug revision used, but for a different, no-longer-buggy
# reason (see that constant's own comment). `canvasX` moved back too.
#
#   * `canvas` sat at x=1024 originally, deep enough into the canvas region
#     that an 80px shift is entirely inside the visible paint gradient --
#     shifted to x=944 for the 64px palette, and now to x=904 for the
#     shrink-to-fit 44px palette. Both were re-derived by bisection against
#     the untouched tests/golden/canvas.png, not guessed, and not assumed to
#     be the mirror image of each other even though 944 -> 904 is exactly
#     the -40 the 64 -> 44 shrink predicts: x=903 and x=905 both land at
#     ~41% mismatched, so x=904 is a real, sharp, single-integer minimum,
#     the same shape the 944 correction found. Unlike that one, x=904's
#     minimum was not exact-zero -- see `view_threshold`'s comment below for
#     the measured residual and why this crop's threshold is no longer 0.
#   * `toolbar` sat at y=77..252 (h=175) originally, and rows 244-252 dipped
#     into the tool palette's new top edge (single-column now, so its first
#     cell is a different tool than the old grid's) -- trimmed to h=166 so
#     the crop stays inside the options bar, which no palette-*width* change
#     touches (confirmed unchanged both times: 167 is still the exact
#     boundary at kToolPaletteW=64 and again at 44, because it depends only
#     on the title/tab-strip/options-bar heights above the palette, never on
#     the palette's width).
#
# `canvas.png` was **not** touched by either move -- `cmp` against `git show
# HEAD:tests/golden/canvas.png` confirms it is still byte-identical to
# sidequest/lucide-toolbox's base commit (72fd411); only its crop *x* moved,
# to look at the same document pixels from each new canvas origin.
#
# `toolbar.png` **was** re-written once, at the first move, but not
# re-captured: the 8 affected rows could not be avoided at h=175 by
# repositioning alone (they show tool-palette content that no longer exists
# in that form anywhere on screen, by design), so the only way to keep every
# remaining pixel provably identical to what was already approved was to
# crop *the reference file itself* down to the unaffected h=166 --
# `goldentool crop` of the old, approved toolbar.png, not a fresh run of the
# app. Verified rather than assumed, both times: a fresh capture at each new
# geometry, cropped to the same h=166, diffs exact-zero against that trim.
# `tools.png` is the only reference this branch has ever re-rendered from a
# live capture, because the tool palette is the only thing it redesigns --
# re-rendered again for this revision's geometry, the same way.
view_crop_x=(0    1916 904  0)
view_crop_y=(77   1075 973  228)
view_crop_w=(1400 640  384  90)
view_crop_h=(166  190  256  270)
view_frames=(90 90 90 90)
view_threshold=(0 96 64 0)
# The second criterion: how many pixels may differ at all, whatever their
# magnitude. See goldentool's runDiff() for why one threshold is not enough.
# `toolbar`/`tools` are 0 because their magnitude threshold is 0 -- there
# the two criteria say the same thing. `layers` is 64: the 90-frame
# measurement's worst observed count was 14 changed px of 121 600, so this
# is ~4.5x that, and still 1400x below the 92 516 px that the diffuse-shift
# test moved. `canvas` is 2624 (4x the measured 656) for the reason given in
# the header comment above `view_names` -- a stable, reproducible sub-pixel
# residual from x=904 not landing on the same physical-pixel phase as the
# original crop, confirmed by repeated spaced-out launches, not the
# load-sensitive false zeros rapid relaunching produced. `tools` is 0 for
# the same reason as `toolbar`: it is static chrome with no animation and no
# simulation behind it, so anything but byte equality is a real change.
# Confirmed by `measure`, not assumed -- see the
# note in cmd_measure on what that mode is for.
view_max_changed_px=(0 64 2624 0)

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
