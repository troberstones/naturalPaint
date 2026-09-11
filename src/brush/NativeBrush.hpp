#pragma once

#include "brush/Grain.hpp"

namespace np {

// brush/NativeBrush -- naturalPaint's OWN section of a brush, beside
// Photoshop's (brush/BrushModel.hpp).
//
// **Why this exists.** `BrushModel` is a lossless mirror of Photoshop's Brush
// Settings panel -- that is its whole value (import fidelity; `--abr-report`'s
// "refused by name" coverage table only works if the model IS the file's
// shape) -- so it must never grow a field Photoshop does not write. But two
// fields naturalPaint has always needed (how much pigment a dab lays down,
// how much water) and two more it grew later (a per-stroke opacity ceiling, a
// paper) have no Photoshop equivalent at all. Before this struct existed they
// sat as four loose scalars/structs directly on `BrushPreset` and `BrushState`
// -- correct, but with no shared name and no single place a fifth
// naturalPaint-only field (stabiliser, taper, paper depth -- Wave 2) could
// join without repeating the same four-line dance on both owners again. This
// struct is that place.
//
// **What may live here:** anything naturalPaint invented that a `.abr` file
// has no descriptor for, and that a brush -- not a stroke, not a tool, not a
// document -- owns. `smudge` (`SmudgeParams`, brush/Smudge.hpp §3b) is NOT
// here even though it is equally naturalPaint's own invention: its own
// comment says why -- it is tool state, one slider that means something
// different for every tool, not a property of the brush being painted with.
// It stays a direct member of `BrushState`.
//
// **What may never live here:** any field Photoshop's Brush Settings panel
// writes. That is what `BrushModel` (brush/BrushModel.hpp) is for, and the
// two files must stay strictly disjoint -- a field in both would be two
// places a save/load round trip could disagree about the same number.
//
// **Where the two are joined.** A brush a painter actually paints with is
// both of these at once: `BrushPreset` and `BrushState` each carry a `model`
// (`BrushModel`) and a `native` (this struct) side by side, copied in
// lockstep by `applyPresetToBrush()`/`presetFromBrush()`
// (app/StrokeSession.cpp) exactly as the two already were as loose fields.
// Nothing merges them into one type: the whole reason they are two fields
// instead of one is that only one of them may ever grow a field Photoshop
// does not also have.
struct NativeBrush {
  // Pigment concentration per dab -- becomes `BrushTip::flow`
  // (`app/StrokeSession::brushTipFor()`). Carried over from `BrushModel`'s
  // own retired comment: naturalPaint's own concept, not Photoshop's, with no
  // equivalent field in `BrushModel`.
  float load = 0.9f;

  // Water per dab -- reaches the solver route only
  // (`StrokeSession::wetnessReachesSolver()`); the CPU layer routes have no
  // water field at all to receive it. Same provenance as `load` above.
  float wetness = 1.3f;

  // The per-STROKE ceiling one stroke can reach on an RGB layer, in [0,1]
  // (brush/RgbDeposit.hpp §2) -- deliberately NOT the same quantity as
  // `load` above, which is how much a single dab lays down. At the default
  // spacing a dab overlaps its neighbours several deep, so a brush that
  // applied its opacity per dab would have no setting that produces a flat
  // partial pass; this is the stroke-level accumulator's own cap instead.
  // 1.0 by default because a brush that does not reach the colour it is
  // loaded with is the surprising case.
  float opacity = 1.0f;

  // Paper tooth (brush/Deposit.hpp §2e, brush/Grain.hpp) -- per brush, until
  // a document-level paper exists. OFF by default, `GrainParams`'s own
  // default; `brushTipFor()` copies it straight into the tip it builds,
  // unscaled by any DYNAMICS target, the same as `opacity` above.
  GrainParams grain;
};

// Bit equality on `load`/`wetness`/`opacity`, `grainParamsEqual()` on
// `grain` -- the same "no tolerance, every value arrives from a slider or a
// preset" convention `brush/Library.hpp`'s `presetMatches()` states for
// itself, extended to this struct now that it holds what that function used
// to compare as four loose arguments.
bool nativeBrushEqual(const NativeBrush& a, const NativeBrush& b) noexcept;

}  // namespace np
