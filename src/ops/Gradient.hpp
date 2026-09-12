#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/Gradient.hpp"
#include "core/SelectionMask.hpp"
#include "core/TileStore.hpp"

// ops/Gradient (PLAN.md "Phase 6 -- Filter and transform it", the tool-shaped
// ops paragraph: "**gradient** (linear, radial, angular, with an editor and
// presets)"; PRD D24, and the gradient half of PRD D26 "fill and stroke a
// selection or layer with colour, pattern or gradient").
//
// The OP only. No editor, no presets, no tool state, no `ui/` anything --
// PRD D24's editor and its saveable presets are a separate piece of work that
// consumes this file, and everything here is headless and testable without a
// GPU, in the same way ops/PointOps and ops/Resample are.
//
// ==========================================================================
// 1-2. The ramp model lives in core/Gradient.hpp
// ==========================================================================
//
// `GradientKind`, `GradientSpread`, `GradientGeometry`, `ColorStop`,
// `OpacityStop`, `GradientStops` and the four pure evaluators are declared
// there, along with the two arguments that shape them: colour and opacity are
// SEPARATE stop lists (§1 there), and interpolation happens in LINEAR light on
// STRAIGHT colour (§2 there). They moved out of this header when
// `core::Document` acquired a gradient table, because a type a Document holds
// cannot sit downstream of core/TileStore.hpp -- core/Gradient.hpp §0 has that
// argument. Nothing about them changed, and this header still includes them,
// so every `#include "ops/Gradient.hpp"` still sees every name.
//
// ==========================================================================
// 3. Dithering: none, and the measurement that says so
// ==========================================================================
//
// PLAN.md's phase 6 does not raise it, but a wide smooth ramp is the textbook
// banding case, so this is a decision and not an omission.
//
// **No dither is applied here.** The output is `core::Tile`'s rgba16float, and
// the argument is arithmetic rather than taste:
//
//   * f16 has an 11-bit effective significand, so its worst-case relative
//     quantisation step is 2^-11. Measured -- by sweeping every binary16 value
//     in [0, 1], taking each gap to its successor, and converting through
//     `color/Space`'s `srgbEncode` times 255 -- **one f16 step spans at most
//     0.08198 of an 8-bit sRGB output code**, i.e. **12.2 f16 steps per output
//     code**. So no f16 quantisation boundary can become an 8-bit boundary,
//     and an 8-bit boundary is what a band is.
//
//     The maximum sits at linear **0.5**, not at 1.0, and the reason is worth
//     a line because the intuitive guess is wrong: 0.5 opens a new binary16
//     binade, so the absolute step there is the same 2^-11 = 4.883e-4 it is
//     just below 1.0, while the sRGB encode's slope is markedly steeper
//     (0.6586 against 0.4396). Same step, worse slope. The value just below
//     1.0 gives only 0.0547 codes.
//
//   * f16 *does* produce plateaus, and pretending otherwise would be the
//     dishonest version of this argument. Measured on a black-to-white ramp
//     sampled at texel centres: 1024 texels wide, no two adjacent texels share
//     an f16 value; **4096 wide, the longest identical run is 2 texels; 8192
//     wide, 4 texels** -- the per-texel linear step (2.441e-4 at 4096) falls
//     below the 4.883e-4 f16 step near white, so the bright end steps and the
//     dark end does not. Those runs are 12x finer than the display grid; they
//     are not visible, and they are not what dither would fix.
//
//   * **The place that needs dither is the 8-bit quantiser, not the
//     generator.** Dithering here would inject noise into float texels that a
//     later blur, resample, grade or composite would then carry and smear --
//     and the export path would quantise the smeared noise to 8 bits anyway,
//     with no dither of its own. If banding is ever reported on an 8-bit PNG
//     export, the fix belongs in io/Export's float->uint8 conversion, where
//     one error-diffusion or blue-noise term fixes *every* smooth image, not
//     only the ones that came from this op.
//
// Rejected: an ordered/Bayer offset added to `t` before evaluation (cheap, and
// the usual trick), because it dithers the *parameter* rather than the value
// and therefore does nothing at all where the ramp is flat, while adding
// spatial noise where it is steep -- exactly backwards.
//
// ==========================================================================
// 4. Selections
// ==========================================================================
//
// `renderGradient()` takes an optional `const Selection*`, and
// core/SelectionMask.hpp's convention is followed exactly, including its
// warning that a per-texel loop which hoists the tile lookup "owns the null-
// Selection branch itself":
//
//   selection == nullptr  ->  coverage 1.0 everywhere. NO RESTRICTION. The
//                             gradient fills the whole region. This is not
//                             "select nothing".
//   a Selection with no tile at a coordinate -> coverage 0.0, outside.
//   a partially covered texel -> the gradient's alpha is SCALED by coverage,
//                             which is what makes an antialiased marquee edge
//                             antialiased in the result rather than a stair-
//                             stepped hard edge.
//
// Coverage multiplies the source **alpha**, and the premultiplied RGB with it
// -- i.e. it scales the whole premultiplied source texel, exactly as
// `clearThroughSelection()` scales by `1 - coverage` for the same reason
// core/SelectionMask.hpp gives there ("scaling all four channels is exactly
// 'this texel is now `1-coverage` as present as it was'"). Scaling alpha alone
// and leaving RGB would produce over-bright premultiplied data, which is the
// fringe in the other direction.
namespace np {

// --------------------------------------------------------------------------
// Rendering
// --------------------------------------------------------------------------

// The document rectangle to fill, in texels, **half-open**: `[x0, x1) x
// [y0, y1)`.
//
// Required rather than inferred, and that is the interesting design point: a
// `TileStore` is sparse and unbounded, so "the layer" has no extent to fill --
// a gradient with no region would either write nothing (there are no tiles yet
// on a blank layer, which is exactly when a user reaches for the gradient
// tool) or write forever. The document's own width/height live in
// `core::Document`, and `core/` above `ops/` is the wrong direction for a
// dependency, so the caller passes the rectangle. PRD D26's "fill a selection"
// passes `selectionBounds()`; "fill a layer" passes the document rectangle.
//
// An empty or inverted rectangle fills nothing and is not an error.
struct GradientRegion {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Renders `stops` through `geometry` into `tiles`, over `region`, weighted by
// `selection`, and composites the result **over** whatever is already there
// (`core::Blend`'s `compositeOver`, reused rather than re-derived so the one
// `over` formula this codebase has asserted at zero tolerance is the one a
// gradient fill uses).
//
// Source-over rather than replace, deliberately: opacity stops only mean
// anything if the backdrop shows through them, and at full opacity with full
// coverage `over` degenerates to exactly a replace, so the "fill this layer
// solidly" case is not paying for the general one. Rejected: a `replace` mode
// alongside, which would differ from `over` only where the source is
// translucent and would therefore be a switch whose two settings look
// identical in the case anyone tests.
//
// `selection == nullptr` means NO RESTRICTION -- the whole region -- per
// core/SelectionMask.hpp. A selection tile that does not exist means coverage
// zero, so a tile no selection covers is skipped without being allocated: a
// gradient through a small marquee on a blank 4K layer costs the marquee's
// tiles, not the document's.
//
// Returns the number of texels actually written, so a caller can distinguish
// "nothing happened because the selection was empty" from "nothing happened
// because the gradient was fully transparent" -- the same reason
// `clearThroughSelection()` returns a count. A texel whose effective alpha is
// exactly zero is not written and not counted, and does not allocate a tile.
size_t renderGradient(TileStore& tiles, const GradientRegion& region,
                      const GradientGeometry& geometry, const GradientStops& stops,
                      const Selection* selection);

}  // namespace np
