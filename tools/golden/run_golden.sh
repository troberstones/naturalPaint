#!/bin/bash
# tools/golden/run_golden.sh -- the golden-image regression harness.
#
# app/Screenshot.hpp makes the app photograph its own swapchain (no macOS
# screen-recording permission, exact rendered pixels -- see that header for
# why). This script is the missing other half: it drives the app through a
# handful of known, scripted UI states via its existing --demo-document /
# --ui-layer-demo / --pigment-stroke-demo / --marquee-demo / --flyout-demo
# CLI flags, crops each capture down
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
# **And with its own empty $NP_PANEL_LAYOUT** (app/PanelLayout.cpp), for the
# same reason and with a far larger blast radius. Since the dockable-panel
# revision the entire chrome -- which panels exist, which dock they are in,
# which are collapsed, and how wide each dock is -- comes out of
# `~/Library/Application Support/naturalPaint/panel-layout.txt`. A developer
# who collapses a panel in the running app has silently changed what every
# golden view captures from then on, and the references in this repo would
# then encode one machine's session rather than the code.
#
# That is not hypothetical: it happened during the revision that added this
# line. Four views came back red, and the cause was that a hand-testing
# session had left COLOR, BRUSH LIBRARY, BRUSH EDITOR and LAYERS collapsed,
# so `layers` had captured an empty dock -- no rows at all -- and the crop
# had been re-aimed twice against that. Pointing the app at a scratch file
# that does not exist makes every capture start from
# `PanelLayout::resetToDefault()`, which is the arrangement the references
# are supposed to be OF. The app may write to this path; nothing reads it
# back, because it is deleted before each capture.
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
#   toolbar, tools, canvas: 0 (exact byte equality). Static chrome for the
#     first two, real simulated paint for the third -- but all three hold
#     at exact equality, because in every case the *content* being compared
#     never actually changes run to run; only where the crop looks at it
#     has moved, four times now, as the tool palette's own geometry has
#     been revised (see the view-table comment below and the block above
#     `view_crop_x` for the full, per-revision account of each move and how
#     it was re-verified). `canvas` briefly carried a wider (magnitude 64,
#     changed-px 2624) tolerance instead of 0 -- a mistake, corrected once
#     rendering the diff image showed the residual was a single ring (the
#     brush-cursor overlay, not the pigment paint) rather than real noise;
#     see that block for the full account. The lesson it left behind:
#     **render the diff before widening a threshold.** A tolerance wide
#     enough to admit that ring would have permanently admitted any
#     regression up to four times its size, on the one view whose whole job
#     (PLAN.md Phase 5's "latent Mix of blue over yellow gives green") is
#     the paint itself -- this harness has now made that exact mistake
#     twice (once at aa5ca57, a diffuse `kChromeBase` shift a magnitude-only
#     criterion missed entirely; once with this view) and corrected both by
#     finding the real cause instead.
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
#   titlebar: exact (0, 0), measured rather than assumed even though this
#     view holds text -- see the view-table comment above for why the fps
#     readout that would otherwise have made that impossible is frozen at
#     the source. Two separate measurement batches, each diffed against its
#     own batch's first launch (197 120 px crop): 11 comparisons across 12
#     launches, then 19 more comparisons across a fresh 20-launch batch -- 30
#     comparisons total, every one 0 mismatched px at max channel diff 0.
#     `toolbar` contains text too and still flakes (one glyph edge,
#     coin-flip rate); this view did not reproduce that in 30 tries, so it
#     is blessed at exact equality the same way `canvas`/`tools`/`flyout`
#     were, rather than borrowing `toolbar`'s (48, 16) out of caution with
#     nothing measured to justify it.
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
#     Lasso, MagicWand and Crop -- sidequest/lucide-toolbox redrew the
#     palette from a 2-wide grid to this single column, then nested it into
#     Photoshop-style flyout groups; see docs/ui.md section 2). Also covers
#     three of the flyout groups' own corner-triangle badges (Move, Lasso,
#     Crop each have more than one member), which is deliberate -- a badge
#     drawn in a colour too close to its background to actually be visible
#     was a real defect caught during this revision (by a pixel-level scan
#     of a screenshot, not by eye -- see the commit message), and this view
#     is what would catch that regression coming back. Icons are merged
#     Lucide glyphs (ui/Fonts.cpp's installToolIconFont()), not hand-drawn
#     ImDrawList vectors, but the same lesson the old comment named still
#     holds: a tool whose icon fails to merge/draw renders as a bare square
#     (or, as a real bug during the original rebuild briefly did, falls
#     back to the wrong drawing entirely -- see ui/Fonts.cpp's
#     installToolIconFont() comment on `config.DstFont`) and nothing else
#     in this project notices. This view is what would have caught it.
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
#   flyout  -- the Brush group's flyout popup under --demo-document
#     --flyout-demo, open over the palette (Brush selected/accent, Pencil
#     dimmed "Not built yet.", Water and Dry Brush plain -- four members,
#     mixed implemented and not, the same reason the user's own instruction
#     named this group "the best subject" for the demo flag). Considered,
#     not assumed: a popup is exactly the kind of UI that *could* animate
#     or land at a mouse-dependent position, either of which would make a
#     byte-equality view flaky by construction -- so this was measured
#     before being blessed, not after. `--flyout-demo` forces the popup
#     open every frame via `ImGui::OpenPopup()` rather than a real click
#     (AppState::openToolFlyoutDemo), and its position is set once via
#     `ImGui::SetNextWindowPos()` anchored to the Brush cell's own screen
#     rect -- both are deterministic given a fixed window size, with no
#     mouse position and no per-frame animation state feeding either one.
#     4 separately-launched captures diffed against the first: 0 mismatched
#     px, max channel diff 0, every time -- exact-zero, the same as
#     toolbar/tools/canvas, so it is blessed at (0, 0) rather than skipped.
#   titlebar  -- docs/reachability-audit.md F2: the y=0..71 title band, never
#     covered before this view -- when the native menu bar landed and removed
#     seven labels from it, all five views of the day still passed. Full
#     window width (0, 0, 2560, 77) under --demo-document, the same demo
#     `toolbar` uses: the naturalPaint wordmark at the left, and at the
#     right, Undo (enabled -- --demo-document leaves an undoable edit on the
#     stack), Redo (disabled) and the fps readout.
#
#     The fps text changes every real run -- `"%.1f fps"`, MacPaintUI.cpp,
#     right-aligned -- so a byte-equality view over the full band could never
#     hold a threshold; it is a different string run to run, not glyph-edge
#     noise like `toolbar`'s. Fixed at the source instead of cropped around:
#     `AppState::screenshotCliActive`, set once for the life of a
#     `--screenshot` run, makes MacPaintUI print a fixed "-- fps" on those
#     frames rather than the live number -- the same reasoning as
#     main.cpp's `(-FLT_MAX, -FLT_MAX)` mouse suppression on screenshot
#     frames, which already covers hover/press tint for this same reason.
#     That is strictly better than cropping short of the readout: it removes
#     a real nondeterminism source rather than cropping around one, and
#     leaves the whole band, not just the wordmark and Undo/Redo, coverable.
#   rail  -- the flyout rail: the strip of one-click panel launchers down the
#     canvas's right edge (ui/MacPaintUI.cpp's `drawPanelRail`), holding the
#     seven View/Simulation panels `app/PanelLayout` puts there on a first run.
#
#     Added because this chrome went from never-populated to default-populated
#     in one revision, and immediately shipped a defect only a screenshot could
#     show: two of its seven cells clipped their last glyph, so GRADE and GRID
#     -- the pair the label rule exists to keep apart -- both read `GR` with a
#     cut third character. The rule itself is headless and was green
#     throughout; what was wrong was that a 24 px button had been written
#     independently of the 28 px rail around it. Nothing in the other seven
#     views frames this strip, so nothing would have caught the next one
#     either. Carries the text thresholds for the same reason `flyout` does:
#     the cells are mono glyphs.
view_names=(toolbar layers canvas tools flyout titlebar transform rail)
view_args=("--demo-document" "--demo-document --ui-layer-demo" "--pigment-stroke-demo" "--demo-document --marquee-demo" "--demo-document --flyout-demo" "--demo-document" "--demo-document --transform-demo 0 --pen-demo" "--demo-document")
# `toolbar`'s height and `canvas`'s x have each moved four times now --
# **their reference PNGs have moved far less**, and this block is the full
# genealogy of both, kept in one place rather than scattered across commit
# messages, because a fifth revision reading only the latest diff would
# have no way to tell a real re-derivation from a copied-forward guess.
#
#   1. sidequest/lucide-toolbox's single-column palette narrowed
#      `kToolPaletteW` from 104 to 64 (docs/ui.md section 2 -- 64, not the
#      44 the very first revision shipped: that number missed that
#      `ImGuiStyle::WindowPadding` is 8px *per side* and left no room at
#      all for the tool grid's then-permanently-visible scrollbar, and
#      rendered every icon clipped in half; see ui/AtelierLayout.hpp's
#      kToolPaletteW for the full account). `canvasX` (= palette width +
#      one rule) moved to x=944.
#   2. The user's own correction -- "make the toolbar fit without
#      scrolling ... the buttons are too large" -- replaced the fixed 36px
#      cell with one computed per frame (ui/AtelierLayout.hpp's
#      atelierToolCellSize()) and dropped the scrollbar entirely, which let
#      `kToolPaletteW` shrink to 44 -- the same numeral revision 1's bug
#      used, but for a different, no-longer-buggy reason. `canvasX` moved
#      to x=904, re-derived by bisection against the untouched
#      tests/golden/canvas.png (x=903/905 both land at ~41% mismatched,
#      x=904 alone is the sharp single-integer minimum, the same shape
#      revision 1's 944 correction found) -- but unlike that correction,
#      x=904's minimum measured a small, stable, non-zero residual (656 of
#      98 304 px, max channel diff 16) rather than exact zero, and this
#      revision's own fix widened `canvas`'s threshold to (magnitude 64,
#      changed-px 2624) to absorb it rather than finding the cause.
#   3. That threshold widening was a mistake, corrected in the very next
#      revision: rendering the diff image (rather than only reading its
#      summary statistics) showed the residual was **one ring** -- the
#      brush-cursor overlay, which follows the pointer in canvas space and
#      therefore moved when the palette narrowed and the canvas band grew.
#      The paint itself, the only thing this view exists to test (PLAN.md
#      Phase 5's "latent Mix of blue over yellow gives green"), was
#      byte-identical throughout. The fix was a crop, not a tolerance: drop
#      canvas's top 64 rows (y 973 -> 1037, h 256 -> 192), which puts the
#      ring outside the frame and restores exact equality (measured: 0 of
#      73 728 px, deterministic across repeated spaced launches).
#      `canvas.png` was trimmed by that identical sub-rect rather than
#      re-captured -- `crop(72fd411:tests/golden/canvas.png, 0, 64, 384,
#      192)`, reproducible and diffable by anyone -- and the threshold went
#      back to (0, 0), which is why it is not 64/2624 today: that budget
#      would have permanently admitted any regression up to four times the
#      ring's own size, on the one view whose whole job is the paint.
#   4. "nest similar tools into a flyout to conserve space like photoshop"
#      collapsed the palette from 28 slots to 18 and bought back enough
#      room to raise `kToolCellMax` from 28 to 36 -- back to this file's
#      very first revision's number -- which raised `kToolPaletteW` from
#      44 to 52. `canvasX` moved to x=920 (re-derived by bisection the same
#      way as before: x=919/921 both land at ~47% mismatched, x=920 is the
#      sharp minimum, and this time it *is* exact zero -- 0 of 73 728 px
#      across 4 spaced launches at (0, 0) -- because the crop already
#      excludes the cursor-ring rows revision 3 found). `canvas.png` itself
#      is untouched by this revision.
#
# `toolbar` moved once, at revision 1: it sat at y=77..252 (h=175)
# originally, and rows 244-252 dipped into the tool palette's new
# single-column top edge -- trimmed to h=166 so the crop stays inside the
# options bar, which no palette-*width* change touches (confirmed
# unchanged at all three later width revisions -- 64, 44, and now 52 --
# because it depends only on the title/tab-strip/options-bar heights above
# the palette, never on the palette's width). `toolbar.png` was re-written
# once, at that same revision, but not re-captured: the 8 affected rows
# could not be avoided at h=175 by repositioning alone (they show
# tool-palette content that no longer exists in that form anywhere on
# screen, by design), so the only way to keep every remaining pixel
# provably identical to what was already approved was to crop *the
# reference file itself* down to the unaffected h=166 -- `goldentool crop`
# of the old, approved toolbar.png, not a fresh run of the app. Verified
# rather than assumed at every later revision too: a fresh capture at each
# new geometry, cropped to the same h=166, diffs exact-zero against that
# trim (revision 4's own check: PASS on 3 spaced runs; one earlier,
# rapid-relaunch check under this same revision showed a transient 4-px
# mismatch that reproduced clean on every immediate spaced retry -- a
# load-sensitive launch artifact, not a geometry regression, the same
# category `canvas`'s own revision-4 re-derivation ran into and resolved
# by spacing launches rather than by widening a threshold).
#
# `tools.png` is the only reference this branch re-renders from a live
# capture at every revision, because the tool palette is the only thing
# each one redesigns -- crop geometry widened from 85x270 (revision 1) to
# 130x270 (revision 2, to clear the then-permanent scrollbar) to 90x270
# (revision 2's later, no-scrollbar shrink-to-fit correction) to this
# revision's 100x350 (taller, to bring two of the new flyout groups' own
# corner-triangle badges into frame -- see the view-table comment above), then
# to 100x402 when the tool palette became a dockable PANEL and grew a 26 px
# grip above its first cell. The extra 52 device px is exactly that grip: the
# crop still starts at the same y, so the five tools the view is defined by
# stay in frame AND the new handle is under coverage rather than beside it.
#
# `layers`' y moved 1075 -> 1209 -> 975, and the middle number is a warning
# worth leaving in place rather than tidying away. 1209 was measured honestly,
# from the highlighted ADJUSTMENT row's own top edge -- **against a capture
# taken from a polluted `panel-layout.txt`** (see the header note on
# $NP_PANEL_LAYOUT). A hand-testing session had left every right-dock panel
# collapsed, so what was actually being aimed at was an empty dock. The
# measurement was sound; its subject was not.
#
# 975 is the same measurement against a hermetic capture, and this time the
# reference was ALSO regenerated -- deliberately, and after rendering the diff
# rather than instead of it. At the re-aimed y the old and new frames differ by
# 1264 px at magnitude 36, all of them in one 20x60 block at the right edge:
# the LAYERS list's scrollbar, which is gone because the panel now has room for
# all five rows without scrolling. That is the property this revision exists to
# produce, so it is a change to accept rather than a drift to chase.
view_crop_x=(0    1916 920  0   0   0    900 1830)
view_crop_y=(77   975  1037 220 700 0    700 230)
view_crop_w=(1400 640  384  100 400 2560 700 100)
view_crop_h=(166  190  192  402 350 77   500 500)
view_frames=(90 90 90 90 90 90 90 90)
# `toolbar` is (48, 16) rather than exact, and the number is measured rather
# than chosen. `run_golden.sh measure 8` on this view returns a BIMODAL
# result -- either 0 px or exactly 4 px, at the same four pixels every time:
# a single 1-px-wide, 4-px-tall vertical run at (88, 33..36), which is one
# glyph stem edge in the tab strip's "Untitled" label. Three of seven pairs
# differed; it is a coin flip, not a rare event, so this view CANNOT hold an
# exact threshold and every run of it was a ~50% chance of a false failure.
#
# Cause is text rasterisation, not layout and not hover: the differing
# pixels are one column of an anti-aliased glyph edge, and neutralising the
# pointer entirely (see main.cpp's screenshot mouse suppression) did not
# change the rate. `canvas` keeps its exact threshold precisely because it
# contains no text at all, which is why it is worth keeping exact.
#
# 48 is ~2x the observed magnitude (23) and 16 is 4x the observed count (4),
# the same shape of margin `layers` uses over its own measurement. The
# changed-px half is doing the real work: a diffuse regression moves
# thousands of pixels, so 16 keeps this view sensitive to everything except
# the one glyph edge it has to tolerate.
#
# **`flyout` carries toolbar's numbers, and that is not laziness -- it is the
# same artifact.** This view was blessed at (0, 0) when it landed, on the
# rule that the palette views contain no text. That rule was wrong about this
# one: the flyout popup lists its members BY NAME ("Brush", "Pencil",
# "Water", "Dry Brush"), so it is a text view wearing a palette view's
# threshold, and it had been passing on luck. Measured 2026-08-24 over eight
# runs: 0-3 changed px at magnitude 5 run-to-run, and one 1-px difference at
# magnitude 18 against a freshly written reference. Same font, same glyph-edge
# rasterisation, same cause as `toolbar` -- so it gets the same bound rather
# than a second number invented for it. `canvas` and `tools` stay exact
# because they genuinely contain no text, and `tools` was re-measured at
# exactly 0 after the palette grew to 28 cells.
view_threshold=(48 96 0 0 48 0 48 48)
# The second criterion: how many pixels may differ at all, whatever their
# magnitude. See goldentool's runDiff() for why one threshold is not enough.
# `tools`/`canvas` are 0 because their magnitude threshold is 0 too -- there
# the two criteria say the same thing, and both were re-measured at exactly 0
# on 2026-08-24. `flyout` is 16, matching `toolbar` for the reason given
# above its own magnitude entry: 16 is over 5x its worst observed count of 3,
# and still leaves the view sensitive to any regression that moves more than
# 0.01% of it. `canvas`
# is not the 2624 an earlier revision of this file used; see the header
# comment above `view_names` for why that was a mistake, corrected. `layers`
# is the one real non-zero budget: 64, from the 90-frame measurement's worst
# observed count of 14 changed px of 121 600, so this is ~4.5x that, and
# still 1400x below the 92 516 px that the diffuse-shift test moved.
# Confirmed by `measure`, not assumed -- see the
# note in cmd_measure on what that mode is for.
view_max_changed_px=(16 64 0 0 16 0 16 16)

# Captures view index $1 (into the app's full-window screenshot, then
# cropped) to path $2, using scratch journal dir $3. Echoes nothing on
# success; returns nonzero and leaves logs in $WORK_DIR on failure.
run_view_capture() {
  local idx="$1" outPng="$2" jdir="$3"
  local name="${view_names[$idx]}"
  local fullPng="$WORK_DIR/${name}.full.png"
  # See the header note: an absent file is what makes the app fall back to
  # `PanelLayout::resetToDefault()`, so this is removed rather than written.
  local layoutFile="$WORK_DIR/${name}.panel-layout.txt"
  mkdir -p "$jdir"
  rm -f "$layoutFile"
  if ! NP_JOURNAL_DIR="$jdir" NP_PANEL_LAYOUT="$layoutFile" \
      "$BIN" ${view_args[$idx]} --screenshot "$fullPng" "${view_frames[$idx]}" \
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
