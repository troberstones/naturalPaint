#pragma once

#include <string>
#include <vector>

namespace np {

// ui/BrushFieldPresentation -- the ONE table that decides what a painter sees
// for each of `BrushModel`'s 151 leaves (brush/BrushModelFields.hpp), and the
// ONE list that accounts for every leaf that gets no control on purpose.
//
// ==========================================================================
// Why this exists
// ==========================================================================
//
// Before this file, a `BrushModel` field could go from "the importer fills
// it in" to "nothing anywhere lets a painter see or change it" with no
// warning: `BrushModelFields.hpp`'s own header calls this out by name
// ("eventually drawing a control per field") as the third thing that walk
// was always going to need to support.
//
// `brushFieldPresentationTable()` below is that control list. Every row's
// `path` is one of the exact strings `brushModelFieldPaths()` produces --
// `ui/MacPaintUI.cpp`'s brush-settings group functions look a leaf's path up
// here, inside their own call to `visitBrushModelFields()`, and draw
// whatever the row says to. `brushFieldOmissionTable()` is the other half:
// every leaf that is deliberately NOT in the table, with one sentence on
// why. `app/selftest/BrushPanelBinding.cpp` asserts that every path from
// `brushModelFieldPaths()` is in exactly one of the two lists -- never both,
// never neither -- which is what turns "a field silently has no control
// anywhere" from a possibility into a test failure.
//
// ==========================================================================
// What is NOT a leaf of its own
// ==========================================================================
//
// `tip.dab.id` and `dual.tip.dab.id` ARE in the presentation table, but as
// `readOnly` string rows: the actual tip picker (`ui/DabPicker`, reached from
// `drawBrushTipShapeGroup()`) writes `AppState::BrushState::dabId` and
// `tipBitmap`, not `BrushModel::tip.dab`, so there is no live widget that
// could safely edit this string today -- typing an arbitrary id here would
// name a bitmap nothing resolved. Shown, not hidden, because the value is
// real and a painter comparing a saved preset against what is loaded should
// be able to see it. `texture.pattern.id`/`.name` are the identical case for
// the (not yet built) pattern picker.
//
// ==========================================================================
// The omission reasons, by shape
// ==========================================================================
//
//   * PsToolOptions's four override `Variance`s (`options.sizeOverride.*`,
//     `.opacityOverride.*`, `.flowOverride.*`, `.colorOverride.*`, 20 paths)
//     -- `PsToolOptions`'s own comment (brush/BrushModel.hpp): parsed and
//     carried, never applied, because how they compose with the brush's own
//     dynamics is not determinable from the file.
//   * `transfer.wetness.*` / `transfer.mix.*` (10 paths) -- `PsTransfer`'s
//     own comment: no engine target for either, because a Pigment texel's
//     seven channels do not include a water value.
//   * `Variance::present` on every Variance this build DOES otherwise show
//     (11 paths: shape.size/angle/roundness, scatter.scatter/countJitter,
//     texture.depthJitter, dual.scatter.scatter/countJitter,
//     color.foregroundBackground, transfer.opacity/flow) -- internal
//     bookkeeping (Variance.hpp's own comment): whether the file said "Off"
//     or said nothing at all, not a fact a painter sets directly. (The
//     `.present` leaves belonging to an ALREADY-omitted Variance -- the four
//     overrides, wetness, mix -- are not listed twice; they are already
//     covered by that Variance's own whole-subtree reason above.)
//   * `tip.computed` / `dual.tip.computed` (2 paths) -- which Photoshop
//     classID the file used (`computedBrush` vs `sampledBrush`,
//     `PsTipShape`'s own comment). Hand-flipping it while `dab.id`/
//     `dab.bitmap` stay whatever they were would produce a model that
//     contradicts itself, and there is no control here that also clears or
//     sets the tip to match -- the same "nowhere sensible to go yet" shape
//     as the override Variances above.
//   * `load` / `wetness` (2 paths, the bare top-level ones -- NOT
//     `transfer.*`) -- `AppState.hpp`'s own comment on `BrushState::load`/
//     `wetness`: a deferred divergence. The stroke reads
//     `BrushState::load`/`wetness` (the Paint tab's Load/Water sliders,
//     `StrokeSession::brushTipFor()`'s `tip.flow = brush.load` and
//     `applyToolToBrush()`'s water write), never `BrushModel::load`/
//     `wetness`. A second live slider bound to the model's copy would look
//     identical to the Paint tab's and silently not be the same number.
//
// Total: 20 + 10 + 11 + 2 + 2 = 45 omitted, 151 - 45 = 106 shown.
//
// ==========================================================================
// `Variance::fadeSteps` -- shown, not omitted, but conditionally
// ==========================================================================
//
// `fadeSteps` is meaningful only when its sibling `control == Fade`
// (Variance.hpp's own comment). Rather than omit it, every group function
// that draws a Variance disables the Fade Steps control -- with a one-line
// reason, the same idiom as everything else in this file -- whenever the
// Variance it belongs to is not currently set to Fade. It is a leaf worth
// keeping visible (an artist switching TO Fade wants the field already
// there) rather than one that should vanish and reappear.

struct BrushFieldSpec {
  const char* path = "";   // must equal one of brushModelFieldPaths()'s strings
  const char* label = "";  // shown beside the control

  // For a float leaf: the slider's printf-style format and range.
  const char* fmt = "%.3f";
  float lo = 0.0f;
  float hi = 1.0f;

  // For an int32_t leaf: the slider's range.
  int iLo = 0;
  int iHi = 100;

  // For a std::string leaf: true draws a disabled/read-only field (the value
  // is real but nothing here can safely resolve an edited one -- see the
  // header above) rather than a free-text box that could name a bitmap or
  // pattern the model has no way to load.
  bool readOnly = false;
};

struct BrushFieldOmission {
  const char* path = "";
  const char* reason = "";
};

// Every leaf that gets a control somewhere. Order is declaration order, not
// meaningful; lookup is always by path.
const std::vector<BrushFieldSpec>& brushFieldPresentationTable();

// Every leaf that deliberately gets none, and why.
const std::vector<BrushFieldOmission>& brushFieldOmissionTable();

// nullptr if `path` names no row in the presentation table.
const BrushFieldSpec* findBrushFieldSpec(const std::string& path) noexcept;

// nullptr if `path` names no row in the omission table; otherwise the reason.
const char* findBrushFieldOmissionReason(const std::string& path) noexcept;

}  // namespace np
