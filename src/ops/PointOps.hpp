#pragma once

#include <array>
#include <functional>
#include <vector>

// ops/PointOps (PLAN.md "Phase 3 -- Grade it", steps 2+3; docs/operations.md
// §1.1 "Committed (PRD D1)"; ADR-0004
// (docs/adr/0004-colour-ops-collapse-to-a-shaper-plus-3d-lut.md)).
//
// Six class-A point ops -- Levels, Curves, Exposure, Saturation,
// RGB -> grayscale, Channel mixer -- as plain `rgb -> rgb` functions, plus
// (step 3, PRD B4) the un-premultiply/re-premultiply wrapper that brackets
// them. docs/operations.md §1.1 is the authoritative spec for what each op
// does (PLAN.md's own step-2 text is "illustrative", per
// DESIGN-imaging.md's note); §1.3 is why shadows/highlights, clarity/local
// contrast and vignette are NOT here (class B, their own spatial pass --
// "even a 3x3 neighbourhood" disqualifies a point op) and dodge/burn is NOT
// here either (class C -- "not an operation, a brush painting into a mask",
// see docs/operations.md §1.3). None of the three appears below.
//
// *** Every function here is `rgb -> rgb`, deliberately ***
// ADR-0004's whole reason for this module to exist as plain composable
// functions rather than methods on some stateful "adjustment" object: a
// later, unbuilt step (color/LutBake) calls a maximal run of adjacent ops
// once per 3-D LUT grid cell (32^3 or 64^3 samples) to bake the whole run
// into one lookup table, and that only works if each op is a pure
// `std::array<float,3> -> std::array<float,3>` function it can chain
// without any of them knowing what runs before or after -- see ADR-0004 and
// PLAN.md step 2's "these are what the LUT baker consumes, so they are
// written once and reused."
//
// Contract shared by all six ops (matches color/Shaper.hpp's own
// established policy for the same reasons):
//  - Operate on straight (non-premultiplied), scene-linear RGB. Never see
//    alpha, never touch it -- alpha is opacity, not light, the same policy
//    color/Space.hpp and core/Probe.hpp already document for the transfer
//    functions. The premultiplied<->straight conversion happens exactly
//    once, in applyPointOpsPremultiplied() at the bottom of this file --
//    never inside an individual op.
//  - Do NOT clamp their output. Scene-linear working-space values routinely
//    exceed 1.0 (HDR-ish highlights), and whether/how a result gets clamped
//    to the LUT's [0,1] domain is color/LutBake's job (a later, unbuilt
//    step), not a policy decision baked into the math here. Levels is the
//    one exception, and it is a narrow one: the *input*-normalized value
//    `t` must be clamped to [0,1] before `pow()` sees it, purely so a
//    below-black input can't hand `pow()` a negative base and produce NaN --
//    that is a mathematical necessity of the formula, not a decision to
//    clamp the op's final output.
//  - No GPU/LUT/texture awareness whatsoever, matching Shaper's own
//    "deliberately narrow and pure" precedent.
namespace np {

// ---------------------------------------------------------------------
// 1. Levels (docs/operations.md: "black/white/gamma per channel and
// composite"). Applied per channel independently -- a "composite" levels
// adjustment is just the caller passing the same LevelsParams for all
// three channels, not a separate code path, per PLAN.md step 2's own
// wording.
//
//   t = clamp((input - blackIn) / max(whiteIn - blackIn, epsilon), 0, 1)
//   t = pow(t, 1 / gamma)
//   output = t * (whiteOut - blackOut) + blackOut
//
// The `max(..., epsilon)` guards a degenerate whiteIn == blackIn from
// dividing by zero; the `clamp` on `t` is the one required-by-math
// exception to this module's no-clamp policy above -- without it, an
// input below blackIn drives `t` negative and `pow()` on a negative base
// is NaN in general (fractional 1/gamma), which would poison every
// channel that touches this pixel downstream.
//
// A visible consequence worth stating plainly: clamping `t` to [0,1]
// also means any input outside [blackIn, whiteIn] saturates to
// blackOut/whiteOut, by construction -- with the neutral defaults below
// (blackIn=0, whiteIn=1), that includes scene-linear HDR headroom above
// 1.0. This is correct, ordinary Levels black/white-point behaviour (the
// same clipping every real levels tool applies outside its own range),
// not the general "ops don't clamp their output" policy being silently
// violated -- a caller who needs Levels to leave HDR headroom untouched
// sets whiteIn above their expected max linear value.
struct LevelsParams {
  float blackIn = 0.0f;
  float whiteIn = 1.0f;
  float gamma = 1.0f;
  float blackOut = 0.0f;
  float whiteOut = 1.0f;
};

// One channel's worth of the formula above. Exposed on its own (rather
// than only reachable through applyLevels()) since it is independently
// hand-checkable and applyLevels() is nothing but this run three times.
float applyLevelsChannel(float input, const LevelsParams& p) noexcept;

// `channels[0..2]` are R, G, B's own independent LevelsParams. Pass the
// same LevelsParams three times for a "composite" adjustment; per-channel
// levels is passing three different ones -- there is no separate
// "composite" struct or code path.
std::array<float, 3> applyLevels(const std::array<float, 3>& rgb,
                                  const std::array<LevelsParams, 3>& channels) noexcept;

// ---------------------------------------------------------------------
// 2. Curves (docs/operations.md: "authored in the shaper domain, not
// linear -- ADR-0004"). A per-channel list of control points, each an
// (x, y) pair whose coordinates are in color/Shaper's log-encoded domain
// (ADR-0004's "format-level commitment" -- once authored, a saved curve's
// numbers ARE shaper-domain coordinates, not linear ones), sorted
// ascending by x. Applying one channel's linear value:
//
//   shaped_in  = shaperEncode(linear)
//   shaped_out = evalCurve(controlPoints, shaped_in)
//   linear_out = shaperDecode(shaped_out)
//
// -- except when that channel has fewer than 2 control points: identity,
// skipping the shaper round-trip entirely (see applyCurves() below).
struct CurvePoint {
  float x = 0.0f;
  float y = 0.0f;
};

// Must be sorted ascending by `.x` -- the caller's contract (a curve UI's
// own control-point order), not something this module re-sorts for you.
using Curve = std::vector<CurvePoint>;

// Cubic Hermite spline through `points`, Catmull-Rom-style tangents
// adapted for non-uniform x-spacing (control points are not evenly
// spaced in general). For the segment `[x_i, x_{i+1}]` containing `x`:
//
//   t = (x - x_i) / (x_{i+1} - x_i)
//   h00 = 2t^3 - 3t^2 + 1
//   h10 = t^3 - 2t^2 + t
//   h01 = -2t^3 + 3t^2
//   h11 = t^3 - t^2
//   y(t) = h00*y_i + h10*(x_{i+1}-x_i)*m_i + h01*y_{i+1} + h11*(x_{i+1}-x_i)*m_{i+1}
//
// Tangents: an interior point's m_i is the average of its two adjacent
// secant slopes; the first and last point instead take the one-sided
// secant slope of their single adjacent segment. At exactly 2 points both
// endpoint tangents reduce to the one shared secant slope, which makes
// this formula reduce exactly to the straight line between them -- a
// property pinned by runPointOpsTest() in app/SelfTest.cpp.
//
// Boundary: `x` outside `[points.front().x, points.back().x]`
// extrapolates flat (returns the nearest endpoint's `.y`), the standard
// curves-tool convention, not a continuation of the spline's tangent.
//
// Degenerate: 0 or 1 control points is identity -- returns `x` unchanged,
// in whatever domain `x` happens to be. (applyCurves() below relies on
// this to skip the shaper round-trip entirely for an unauthored channel,
// rather than relying on shaperDecode(shaperEncode(x)) == x to only
// approximate float tolerance.)
float evalCurve(const Curve& points, float x) noexcept;

// Per channel: shaperEncode -> evalCurve -> shaperDecode, or a bare
// passthrough when that channel's curve has fewer than 2 points (see
// evalCurve()'s degenerate-case doc comment above for why the shaper
// round-trip is skipped rather than relied on in that case).
std::array<float, 3> applyCurves(const std::array<float, 3>& rgb,
                                  const std::array<Curve, 3>& channels) noexcept;

// ---------------------------------------------------------------------
// 3. Exposure (docs/operations.md: "stops; a pure multiply in linear").
// Deliberately linear-light, NOT the shaper domain -- unlike Curves, this
// is not authored against perceptual/log-domain coordinates, it is a
// physical light multiply, per docs/operations.md's own spec.
//
//   output = input * 2^stops
//
// `stops = 0` is identity.
struct ExposureParams {
  float stops = 0.0f;
};

std::array<float, 3> applyExposure(const std::array<float, 3>& rgb,
                                    const ExposureParams& p) noexcept;

// ---------------------------------------------------------------------
// Rec.709 luma weights -- the exact three literal constants
// shaders/grayscale_blit.wgsl already hardcodes for the GPU grayscale-
// preview pass (`dot(c.rgb, vec3(0.2126, 0.7152, 0.0722))`). Reproduced
// here verbatim, not re-derived or re-rounded, and shared as the default
// weight for both Saturation and Grayscale below, so the CPU-side grading
// path's luma and the GPU-side display-preview's luma can never
// numerically disagree.
inline constexpr std::array<float, 3> kRec709LumaWeights{0.2126f, 0.7152f, 0.0722f};

// `weights[0]*r + weights[1]*g + weights[2]*b`. Shared helper behind both
// Saturation and Grayscale below, so their luma computations are
// literally the same code, not two hand-copies of the same dot product.
float computeLuma(const std::array<float, 3>& rgb,
                   const std::array<float, 3>& weights = kRec709LumaWeights) noexcept;

// ---------------------------------------------------------------------
// 4. Saturation (docs/operations.md: "against a stated luma weight, not a
// naive average"). Weights are a real parameter (docs/operations.md D1:
// "weights should be exposed for both this and grayscale"), defaulting
// to kRec709LumaWeights above.
//
//   luma = weights . rgb
//   output[c] = luma + (input[c] - luma) * scale
//
// `scale = 1.0` is neutral/identity, `scale = 0.0` collapses every
// channel to `luma` (grayscale), `scale > 1.0` oversaturates.
struct SaturationParams {
  float scale = 1.0f;
  std::array<float, 3> lumaWeights = kRec709LumaWeights;
};

std::array<float, 3> applySaturation(const std::array<float, 3>& rgb,
                                      const SaturationParams& p) noexcept;

// ---------------------------------------------------------------------
// 5. RGB -> grayscale (docs/operations.md: "Rec.709 luma default, weights
// exposed"). The same luma dot product as Saturation above -- same
// default weights, same shared computeLuma() -- replicated to all three
// output channels, since this codebase has no single-channel image
// representation yet (core/Layer.hpp: only LayerKind::RGB has populated
// tile storage today).
//
//   luma = weights . rgb
//   output = (luma, luma, luma)
struct GrayscaleParams {
  std::array<float, 3> lumaWeights = kRec709LumaWeights;
};

std::array<float, 3> applyGrayscale(const std::array<float, 3>& rgb,
                                     const GrayscaleParams& p) noexcept;

// ---------------------------------------------------------------------
// 6. Channel mixer (docs/operations.md: "3x4 matrix including offsets").
// `matrix[i]` is output channel i's row: `{rWeight, gWeight, bWeight,
// offset}`.
//
//   output[i] = matrix[i][0]*r + matrix[i][1]*g + matrix[i][2]*b + matrix[i][3]
//
// Identity/neutral: matrix[i] picks out channel i with a zero offset,
// the default below.
struct ChannelMixerParams {
  std::array<std::array<float, 4>, 3> matrix{{
      {1.0f, 0.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f, 0.0f},
  }};
};

std::array<float, 3> applyChannelMixer(const std::array<float, 3>& rgb,
                                        const ChannelMixerParams& p) noexcept;

// ---------------------------------------------------------------------
// Step 3 (PRD B4): "Alpha is premultiplied; per-channel colour ops
// un-premultiply and re-premultiply." The only place premultiply/
// un-premultiply happens for the grading path -- every op above must
// never see premultiplied data and must never know this wrapper exists,
// which is what keeps them the pure, LUT-bakeable functions ADR-0004
// needs.
//
// One point-op function, straight linear RGB -> straight linear RGB --
// exactly what the six ops above already are. Callers compose a run of
// them (a lambda, or one of the six functions bound to its params) into
// as many PointOp entries as they need; this wrapper applies them in
// list order and otherwise knows nothing about ordering semantics --
// that composition/ordering policy belongs to core::OpStack, a separate,
// later, unbuilt step.
using PointOp = std::function<std::array<float, 3>(const std::array<float, 3>&)>;

// `premultiplied` is one texel exactly as core::Tile::readPixel() returns
// it -- premultiplied RGBA, scene-linear.
//
// 1. Un-premultiply, mirroring core/Probe.cpp's unpremultiply() guard
//    exactly: `a <= 0` -> `{0,0,0,0}` (nothing to grade, alpha stays 0 --
//    the same value an untouched core::Tile texel already reads).
//    Otherwise `straight_rgb = premultiplied_rgb / a`.
// 2. Run `ops` on `straight_rgb`, in order.
// 3. Re-premultiply: `output_rgb = graded_rgb * a`, alpha unchanged.
//
// Alpha is never modified, on the strength of an invariant confirmed
// against docs/operations.md §1 before writing this wrapper: no point op
// in the committed P0 set (Levels, Curves, Exposure, Saturation,
// grayscale, channel mixer) ever touches alpha -- every one of them is
// colour-only by definition (a "3x4" channel mixer matrix is 3 rows, RGB
// output only, no 4th row for alpha; grayscale replicates luma across
// RGB, never alpha). If a future op ever needs to modify alpha, it does
// not belong behind this wrapper.
std::array<float, 4> applyPointOpsPremultiplied(const std::array<float, 4>& premultiplied,
                                                 const std::vector<PointOp>& ops);

}  // namespace np
