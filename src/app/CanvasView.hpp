// Pure view state and guide geometry: the small value types app/Snapping.hpp,
// app/ViewTransform.hpp and the --selftest sections need without the rest of
// app/AppState.hpp.
#pragma once

namespace np {

// PLAN.md Phase 2 step 11 ("View controls", PRD Q1-Q4): zoom/pan plus
// independent axis mirrors, a rotation angle and a grayscale-preview toggle.
// All four of the new fields below are *view* state -- nothing in
// core/Document, core/Layer or sim/PaintSim ever reads a CanvasView, so
// flipping any of them can't touch the document by construction, not just by
// convention (--selftest's runViewTransformTest() asserts this for the one
// field, grayscale, that does reach into GPU state at all).
//
// zoom/panX/panY keep their original meaning unchanged (NaturalPaintUI.cpp's
// tileScreenRect() -- pure tile-to-screen placement geometry with no need
// for mirror/rotate -- reads only these three fields and is untouched by
// this step). mirrorX/mirrorY/rotation are composed
// together with zoom/pan into one affine transform by app/ViewTransform.hpp;
// see that header and ui/MacPaintUI.cpp's canvas block for where pen input
// maps back through that transform's actual analytic inverse, per
// docs/shortcuts.md section 3's own mandate ("mirror as a special case in
// the draw path is what makes painting-under-mirror land in the wrong
// spot" -- PLAN.md).
struct CanvasView {
  float zoom = 1.0f;
  float panX = 0.0f;
  float panY = 0.0f;
  bool mirrorX = false;   // PRD Q2: left/right, independent of mirrorY
  bool mirrorY = false;   // PRD Q2: up/down, independent of mirrorX
  float rotation = 0.0f;  // PRD Q4: radians, arbitrary angle, about canvas centre
  // PRD Q3: a per-pixel luminance pass (sim/PaintSim's updateGrayscalePreview
  // + shaders/grayscale_blit.wgsl), not a geometric part of the transform --
  // kept here anyway because it is still pure view state, never document
  // state.
  bool grayscale = false;
  // PLAN.md Phase 3 step 6 ("Apply pass") / step 8 ("Op-stack UI"): the
  // grading preview toggle, the same "pure view state, never document
  // state" shape as grayscale immediately above -- but unlike grayscale (a
  // self-contained GPU blit with no other inputs), this one reaches into
  // AppState::opStack and sim::PaintSim's bake/blit pipeline
  // (PaintSim::updateGradePreview() + shaders/grade_blit.wgsl) rather than
  // shaders/grayscale_blit.wgsl alone. User-visible and explicit, exactly
  // like grayscale: grading does NOT silently switch on just because
  // st.opStack becomes non-empty -- a user builds a stack via the GRADE
  // section's real op-authoring UI (ui/MacPaintUI.cpp, step 8) and flips
  // this on separately to preview it, via the "Preview Graded Output"
  // checkbox at that section's top (the one and only way to toggle it --
  // step 6's earlier "Test Grade (debug)" View-menu item, which hardcoded
  // two fixed op indices main.cpp no longer seeds, is gone).
  bool grade = false;
};

// PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD Q5-Q7).
// A guide is a single line pinned at one document-space coordinate along the
// axis *perpendicular* to its own orientation -- a Horizontal guide is a
// horizontal line at a fixed Y, a Vertical guide a vertical line at a fixed
// X, matching Photoshop's own naming. Drawn (app/Snapping.hpp's
// resolveSnap(), ui/MacPaintUI.cpp) through the same ViewTransform every
// other document-space thing on the canvas goes through -- never a second,
// screen-space-only position.
enum class GuideOrientation { Horizontal, Vertical };

struct Guide {
  GuideOrientation orientation = GuideOrientation::Horizontal;
  float position = 0.0f;
};

}  // namespace np
