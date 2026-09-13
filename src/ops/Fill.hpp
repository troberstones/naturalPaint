#pragma once

#include <array>
#include <cstdint>

#include "core/Blend.hpp"
#include "core/Gradient.hpp"
#include "core/TileStore.hpp"
#include "ops/Pattern.hpp"
#include "ops/Roi.hpp"

// ops/Fill -- PRD D26's engine: fill (and, through the same engine, stroke)
// with colour, a defined pattern, or a gradient, through an arbitrary blend
// mode and opacity.
//
// **Why this is one engine and not three.** `app/CommandsFill.cpp`'s `fill`
// and `stroke` commands both need "the source's colour at this texel, blended
// over what was already there" -- stroke asks it only where a band around the
// selection's edge says to, fill asks it everywhere the selection's own
// coverage says to. Both are answered by `app/FilterOps.hpp`'s
// `compositeFilterResult()` lerping this engine's full-canvas output against
// the original by a coverage field -- fill's is `OpenDocument::selection`
// itself (so it plugs straight into `app/PixelOpBridge.hpp`'s
// `applyPixelFilter()`, exactly as every other menu pixel op does); stroke's
// is a band `core/SelectionRefine.hpp`'s grow/shrink builds from that same
// selection. Building a second engine for stroke would be two answers to "what
// colour does this fill put down", which is the drift every shared-engine
// header in this codebase (`app/PixelOpBridge.hpp`, `app/FilterOps.hpp`) is
// built to prevent.
//
// **The gradient case reuses `ops/Gradient::renderGradient()` rather than a
// second ramp evaluator.** That function always composites its own result
// `over` a live store; called here with a fresh, empty scratch `TileStore` and
// no selection, `over` on a transparent backdrop is an exact identity
// (`core/Blend.hpp`'s own stated identity), so the scratch ends up holding
// nothing but the ramp's raw premultiplied samples -- which is exactly what
// this file blends through `blend`/`opacity` afterwards. One evaluator, one
// caller of it, and the fill's own blend mode and opacity are layered on top
// rather than reimplemented.
namespace np {

// Which of PRD D26's three contents a fill or stroke draws from. "Foreground
// colour" is not a fourth member here: `ui/`'s dialog resolves the current
// foreground swatch into a concrete `FillParams::color` at the moment it
// builds the command (app/GradientTool.hpp's own resolved-vs-authored split,
// one level over) -- a command is a function of an `OpenDocument` alone
// (app/Command.hpp §1), and the foreground swatch is `AppState`, not the
// document.
enum class FillSource {
  Color,
  Pattern,
  Gradient,
};

struct FillParams {
  FillSource source = FillSource::Color;

  // Straight (non-premultiplied) linear RGB, opaque -- the working space's own
  // convention for an authored colour (`ops/Gradient.hpp`'s `ColorStop`, one
  // level over). Read only when `source == Color`.
  std::array<float, 3> color{0.0f, 0.0f, 0.0f};

  // Read only when `source == Pattern`. Not owned; null is refused rather than
  // treated as "fill with nothing", `PatternFillParams`'s own rule.
  const Pattern* pattern = nullptr;
  int32_t patternOriginX = 0;
  int32_t patternOriginY = 0;

  // Read only when `source == Gradient`.
  GradientGeometry gradientGeometry{};
  GradientStops gradientStops{};

  // How the source combines with what is already on the layer. `Mix` is
  // refused by `fillParamsValid()`: it is a Kubelka-Munk lerp between two
  // Pigment layers' latents (`core/Blend.hpp`), and a fill's source is a
  // colour, a pattern or a gradient -- none of which has a latent to offer.
  BlendMode blend = BlendMode::Normal;

  // Scales the source's own coverage before the blend -- the dialog's
  // "Opacity" field, distinct from a gradient's own opacity stops or a
  // pattern's own alpha. 0 paints nothing; 1 is the source's own coverage
  // unchanged.
  float opacity = 1.0f;
};

// `opacity` finite and non-negative, `blend != Mix`, and the source's own
// payload present (`color` finite, `pattern` non-null and valid, or nothing
// further for `Gradient` -- `ops/Gradient.hpp`'s own stated rule that zero
// colour stops is a legitimate "renders nothing" ramp, not an error).
bool fillParamsValid(const FillParams& p) noexcept;

// The engine, in `app/PixelOpBridge.hpp`'s dispatch shape: every texel of
// `outRect` in `dst` is REPLACED with `blendPixel(p.blend, source, original)`,
// where `source` is this fill's own premultiplied sample (colour, pattern or
// gradient, each scaled by `p.opacity`) and `original` is read from `src`.
// `dst == nullptr || dst == &src`, an empty `outRect`, or an invalid `p`
// refuse (return false) without writing anything, matching
// `patternFillTiles()`'s and every other engine sharing this signature's own
// contract.
//
// Callers apply the selection (or a stroke's own band, §ops/Fill's header)
// afterwards, by lerping this function's full-`outRect` output against `src`
// through `compositeFilterResult()` -- this function itself takes no
// selection and knows of none, for `app/FilterOps.hpp`'s own reason: the
// engine runs everywhere, and the caller decides where the result lands.
bool fillTiles(const TileStore& src, const PixelRect& outRect, const FillParams& p,
               TileStore* dst);

}  // namespace np
