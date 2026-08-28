#pragma once

// ui/DynamicsMatrixPanel -- the shelved 10x12 source->target LINK MATRIX
// editor (brush/Dynamics.hpp's `BrushLinkSet`), behind `--advanced-dynamics` /
// `AppState::showAdvancedDynamics` now that brush/BrushModel (Photoshop's own
// shape, decoded straight off the `.abr` file) is what actually paints
// (app/StrokeSession.cpp's `brushTipFor()` and its per-dab loop).
//
// **Why this file exists rather than the two functions staying inline in
// `ui/MacPaintUI.cpp`.** They did not move because they changed -- they did
// not, this is a cut-and-paste extraction -- they moved because
// `ui/MacPaintUI.cpp` is 11,000+ lines and this project's own convention is
// "move code out, never in." Gating the two call sites at once, here, is also
// what keeps the matrix's UI from being reachable by accident: a stray
// `CollapsingHeader` left un-gated during some later edit to that file would
// be a much easier mistake to make than a stray call into a header that says
// on its own first line what it is for.
//
// **The matrix is shelved, not deleted.** `brush/Dynamics.{hpp,cpp}` are
// untouched by this migration -- `BrushLinkSet`, `evaluateLinks()`,
// `multiplyFloor`, every selftest that exercises them directly, all stay
// exactly as they were. What changed is that nothing in the PAINT path reads
// `BrushState::links`/`BrushPreset::links` any more (brush/BrushModel.hpp's
// own header names this as the whole argument for the replacement); the
// matrix and its editor become write-only-by-the-shelf, i.e. this panel is
// now the only thing that still writes to a `BrushLinkSet`, and nothing reads
// one back to paint with.
//
// **The three-edit path back**, as far as this migration goes and no
// further, should the matrix ever need to drive a stroke again:
//
//   (a) default `AppState::showAdvancedDynamics` to `true`, or add a menu
//       item that flips it -- today it is `false` and this panel is
//       reachable only via the `--advanced-dynamics` CLI flag;
//   (b) restore the in-range `link`/`floor` parse branches in
//       `app/UserBrushLibrary.cpp` that this migration's Part 4 turned into
//       an unconditional preserve-verbatim (every `link`/`floor` line is
//       carried as an opaque, unparsed line today, regardless of whether its
//       ordinal is in range) -- without this, a saved link never becomes a
//       live `BrushLink` again on load, however far (a) is pushed;
//   (c) fold the matrix's resolved `DynamicResult` into the Variance-driven
//       result MULTIPLICATIVELY, downstream of the Variance pass, inside
//       `brushTipFor()`/`StrokeSession`'s per-dab loop -- not in place of it.
//       `brush/Variance.hpp`'s own header is why: Variance's floor is applied
//       exactly once, inside its own formula, and re-introducing a second,
//       independent multiplier onto the same product from the matrix is
//       exactly the shape of the B6 defect Variance exists to make
//       unrepresentable, unless it composes strictly after Variance's own
//       floor has already been applied rather than before or in its place.
//
// None of the three is done here.

namespace np {

struct AppState;

// The DYNAMICS matrix: every source against every target it could drive.
// Verbatim from `ui/MacPaintUI.cpp` -- see the function's own header comment,
// carried over unchanged, for the design argument ("an empty cell is as
// informative as a filled one -- you can see that nothing drives spacing").
void drawDynamicsMatrix(AppState& st);

// The LINK editor: one cell's response curve, its range, and what it is
// resolving to right now. Verbatim from `ui/MacPaintUI.cpp`.
void drawLinkEditor(AppState& st);

}  // namespace np
