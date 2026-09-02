#pragma once

#include <string>

#include "core/Blend.hpp"

// brush/ToolOptionsBlend -- the one edge that maps Photoshop's own tool-
// options blend id (`PsToolOptions::blendMode`, `Md ` on disk) onto this
// project's layer-compositing vocabulary, `core::BlendMode`.
//
// **Part 3's whole deliverable.** `PsToolOptions::blendMode` is parsed and
// read by nothing (brush/BrushModel.hpp's own comment: "Present on all 101
// presets and read by nothing... 40% of these brushes are not Normal-mode
// brushes at all"). Measured distribution across 101 presets: Normal 61,
// Darken 20, Multiply 16, Linear Burn 2, Dissolve 1. This function is the
// mapping; `BrushTip::blend` (brush/Deposit.hpp) is where `brushTipFor()`
// stores the result.
//
// **Not wired into any deposit route.** `brushTipFor()`'s own comment on
// `BrushTip::blend` names the two obstacles a bounded investigation found:
// a Pigment texel has no premultiplied RGBA for `core::blendPixel()` to
// blend (`core::Pigment.hpp` stores a straight latent plus a mass, and
// `rgbToLatent()` -- the only way back from a blended RGBA to a latent --
// is documented elsewhere in this codebase as "plausible rather than true",
// which is not the reading this project uses for anything that paints), and
// Photoshop's own Eraser tool does not consult a brush's blend mode at all,
// so wiring `tip.blend` into either erase route would be inventing a
// behaviour Photoshop itself does not have. The mapping and the field are
// left in place, harmless and unused, as groundwork.
//
// **`core::BlendMode` itself is not grown for this.** It is the layer-
// compositing vocabulary, serialized into the document file format, and an
// earlier planning document for this project says directly that widening it
// for a per-stroke concept is the wrong move. This function maps ONTO the
// existing enum instead.
namespace np {

// Maps `psId` (Photoshop's own `Md ` descriptor id, e.g. "Nrml"/"Mltp"/
// "Drkn"/"linearBurn"/"Dslv") onto a `core::BlendMode` this build can
// composite. Returns `true` and fills `out` for every id this build can
// honour; returns `false` and leaves `out` untouched otherwise, filling
// `reasonOut` (when non-null) with a human-readable refusal reason -- this
// project's "disabled with a reason" idiom (see `PixelOpRefusal`,
// app/StrokeSession.hpp).
//
// The empty string -- a brush with no `toolOptions` block, or one authored
// before this key existed -- maps to `BlendMode::Normal`, the same fallback
// an id this build has never seen also gets when the caller ignores the
// `false` return (`brushTipFor()`'s own use: "fall back to Normal on
// refusal, same as today's implicit behaviour").
//
// **`"Drkn"` (Darken) maps to `BlendMode::Min`, not a new enumerator.**
// `brush/CoverageBlend.cpp`'s own `applyCoverageBlend()` already establishes
// the identical equivalence for the Dual Brush/Texture blend table --
// `CoverageBlend::Darken` is implemented as `std::min(a, b)` there, on a
// bare coverage scalar. The same arithmetic identity holds componentwise on
// premultiplied RGBA: Darken keeps whichever of source and destination is
// darker, channel by channel, which is exactly `core::BlendMode::Min`
// (`core/Blend.hpp`'s own PRD B7 classification: scene-linear, no built-in
// reference white). Reused here rather than re-derived.
//
// `"linearBurn"` and `"Dslv"` (Dissolve) are refused by name: naturalPaint
// has no per-stroke blend implementation for either (Linear Burn would need
// a new `core::BlendMode` this task deliberately does not add; Dissolve is a
// per-pixel random threshold, not a deterministic two-colour blend, and has
// no formula in `core/Blend.hpp` at all). Any id outside this build's five
// known Photoshop spellings is refused the same way, naming the unknown id
// rather than guessing at it.
bool blendModeFromPsToolOptions(const std::string& psId, BlendMode& out,
                                std::string* reasonOut = nullptr);

}  // namespace np
