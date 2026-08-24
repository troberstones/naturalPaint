#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

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
// 1. Colour stops and opacity stops are two lists, not one
// ==========================================================================
//
// The tempting model is a single list of RGBA stops. It is wrong, and it is
// expensive to be wrong about, so the reason is stated before the types.
//
// **Every real gradient editor positions colour and opacity independently**
// (Photoshop's gradient bar has stops above the ramp and stops below it, and
// they do not share positions; SVG's `stop-color`/`stop-opacity` are separate
// attributes on the same stop only because SVG has no editor). A designer
// authoring "this colour ramp, fading out over the last third" wants two
// opacity stops at 0.66 and 1.0 and no new colour stop at 0.66 -- and in a
// merged model, adding that opacity stop forces a colour stop at the same
// place, whose colour must then be computed and *frozen*. Move a neighbouring
// colour stop afterwards and the frozen one no longer follows: the ramp
// visibly kinks at a position the user never authored. Merging is not a
// simplification, it is a loss of authoring intent, and unpicking it later
// means migrating every saved preset.
//
// The second consequence is the one that shows up as a *pixel* bug, and it is
// the answer to "what happens with premultiplied stops":
//
//   **A separated model has no such thing as a transparent-black stop.**
//   Colour is defined at every `t` by the colour ramp alone, regardless of
//   what the opacity ramp says there. A merged model lets a user author
//   `(0,0,0,0)` at the transparent end -- which is what a colour picker hands
//   back when the alpha slider hits zero -- and then interpolating toward it
//   drags the RGB toward black under the fade. That grey-then-black band
//   under a fading white ramp is the classic "dark fringe", and this model
//   makes it unauthorable rather than merely discouraged.
//
// ==========================================================================
// 2. Interpolation happens in the LINEAR working space, on STRAIGHT colour
// ==========================================================================
//
// --- Linear, not display-encoded -----------------------------------------
//
// DESIGN-imaging.md's §2 commitment is unconditional: "decode to a linear
// working space on import, do all work there". `core::Tile` is premultiplied
// linear rgba16float, `ops/PointOps` grades in linear, `core/Blend` composites
// in linear, `ops/Resample` filters in linear. A gradient interpolated in
// sRGB-encoded values would be the only pixel-producing path in the build
// whose *result* depends on the display encoding, and the encoding is a
// per-document, per-export choice (io/ExportAs picks a space at write time),
// so the same gradient would have to change when the user changed their
// export target. That alone settles it.
//
// **The consequence, stated out loud because it is visible and users will
// notice it.** A black-to-white linear-light ramp does not look like an even
// ramp of brightness. Linear 0.5 -- the geometric midpoint of a black-to-white
// gradient -- displays as sRGB 0.735357 (measured through `color/Space`'s own
// `srgbEncode`), i.e. a light grey; and the ramp only reaches perceptual
// middle grey at linear 0.214041, which is 21.4 % of the way across. That is what
// physically-even light *is*; it is also not what Photoshop does by default,
// and a user coming from Photoshop will call it wrong. Two things make it
// livable, and neither is "interpolate in sRGB after all":
//
//   * `ColorStop::midpoint` (below) is the standard per-segment skew control
//     every gradient editor already has. Setting the black->white segment's
//     midpoint to 0.7322 puts perceptual middle grey back at the geometric
//     middle of the ramp -- measured: that midpoint makes t = 0.5 evaluate to
//     linear 0.21408, against the 0.214041 that `srgbDecode(0.5)` wants. The
//     number is not `srgbEncode(0.5)` = 0.735357, which is the near miss to
//     expect here; the two would coincide only if sRGB were a pure power law,
//     and its linear toe is why they do not.
//   * Extra colour stops. A three-stop ramp is the normal way to shape a
//     gradient and costs nothing here.
//
// Rejected: interpolating in `color/Space`'s sRGB encoding (matches Photoshop,
// looks "even" for the black-to-white case, and breaks every other case --
// it makes the op's output depend on a display transform, and a subsequent
// blur or resample of a display-encoded gradient is then wrong for exactly
// the reason DESIGN-imaging.md gives). Also rejected: interpolating in Oklab
// or a similar perceptual space, which would give genuinely nicer hue
// transitions but introduces a second working space into a file whose job is
// to write texels into a linear tile store, and which no other op in this
// build speaks. If a perceptual ramp is wanted later it belongs as an
// explicit `GradientInterpolation` enum on the stop list, added when a UI
// exists to expose it -- not as a silent default.
//
// --- Straight colour, premultiplied only at the write ---------------------
//
// `ColorStop::color` is **straight (unassociated)** linear RGB, and the
// premultiply happens exactly once, in `renderGradient()`, after the colour
// and opacity ramps have each been evaluated. Interpolating premultiplied
// stop values instead gives a different answer through any fade, and a worse
// one:
//
//   Take opaque red `(1,0,0,1)` fading to transparent blue `(0,0,1,0)`.
//   Straight, at t = 0.5:  colour (0.5, 0, 0.5), opacity 0.5 -- a half-strength
//                          magenta, the colour ramp's own midpoint. Stored
//                          premultiplied: (0.25, 0, 0.25, 0.5).
//   Premultiplied, at 0.5: (0.5, 0, 0, 0.5), which un-premultiplies to pure
//                          red. The blue stop contributed *nothing*, because
//                          its premultiplied form is (0,0,0) and carries no
//                          hue at all. **The blue channel differs by 0.25 out
//                          of a 0.25 correct value -- a 100 % error, not a
//                          rounding difference.**
//
// So premultiplied interpolation silently weights each stop's colour by its
// own opacity, which means the colour ramp stops being a colour ramp as soon
// as the opacity ramp is non-constant -- destroying the independence §1 exists
// to provide. (This is the same operation as the dark fringe: in the merged
// model the transparent stop is black and drags the result dark; here the
// transparent stop is blue and gets ignored. Both are "premultiplied values
// are not colours".) The premultiplied convention is right for *storage* and
// for *compositing* -- core/Blend.hpp and core/SelectionMask.hpp both depend
// on it -- and wrong for *authoring*, and this file is the boundary.
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
// Geometry
// --------------------------------------------------------------------------

// The three PRD D24 names. Each is a different function from a document
// position to the ramp parameter `t`; nothing else about the op differs
// between them, which is why they are an enum and not three entry points.
enum class GradientKind {
  // t = the projection of the point onto the segment p0->p1, as a fraction of
  // that segment's length. Constant along every line perpendicular to p0->p1.
  Linear,
  // t = |p - p0| / |p1 - p0|. p0 is the centre, p1 sits on the t = 1 circle.
  // Circular, not elliptical: an elliptical radial needs a second axis and a
  // rotation, which is a transform on the input point and belongs to phase 6's
  // transform machinery rather than to a third geometry here.
  Radial,
  // t = the angle of (p - p0) measured from the direction p0->p1, swept once
  // around and wrapped into [0, 1).
  //
  // **The sweep direction is clockwise on screen**, and that is a consequence
  // rather than a preference: document space is y-DOWN (core/Tile.hpp's
  // coordinates, and every raster in this build), so an `atan2(dy, dx)` that
  // increases counter-clockwise in maths convention increases clockwise once y
  // points down. Stated because the alternative -- silently negating to get
  // "counter-clockwise like the maths textbook" -- would make the op disagree
  // with the direction a user drags the handle.
  Angular,
};

// What happens outside [0, 1]. Only `Linear` and `Radial` can leave the range
// (`Angular` wraps by construction, and ignores this field).
enum class GradientSpread {
  // Clamp to the end stops. The default, and what every gradient tool does
  // unless told otherwise.
  Pad,
  // t -> t - floor(t). Tiles the ramp; discontinuous at every integer unless
  // the first and last stops match.
  Repeat,
  // Triangle wave: ping-pongs, so the ramp is continuous at every integer
  // regardless of what the end stops are. Almost always what a user who asked
  // for "repeat" and got a hard seam actually wanted.
  Reflect,
};

// Where the gradient is, in document texel coordinates.
//
// The two points mean different things per kind (start/end, centre/rim,
// centre/zero-angle) but are one pair in all three, because they are one pair
// in the UI too: a gradient is dragged, and the drag has a start and a finish.
struct GradientGeometry {
  GradientKind kind = GradientKind::Linear;
  float x0 = 0.0f, y0 = 0.0f;
  float x1 = 0.0f, y1 = 0.0f;
  GradientSpread spread = GradientSpread::Pad;
};

// --------------------------------------------------------------------------
// The stop model
// --------------------------------------------------------------------------

// One colour stop: a position on the ramp and a STRAIGHT linear RGB colour.
//
// Straight, not premultiplied -- see §2. And linear: a colour picked from an
// sRGB swatch must be decoded (`color/Space.hpp`'s `srgbDecode`) by whoever
// builds the stop list, not here, for the same reason `ops/PointOps` never
// decodes: the op operates in the working space and the working space is
// linear.
struct ColorStop {
  // Ramp position. Conventionally in [0, 1], but not clamped or validated:
  // `Repeat`/`Reflect` are defined for any real `t`, and a stop list whose
  // positions run 0..100 works and simply means the caller's `t` does too.
  float position = 0.0f;

  // Straight, scene-linear RGB. Not clamped to [0, 1]: DESIGN-imaging.md's
  // working space is scene-referred and a gradient into an HDR highlight is a
  // legitimate thing to author. `ops/PointOps` takes the same no-clamp
  // position for the same reason.
  std::array<float, 3> color{0.0f, 0.0f, 0.0f};

  // Where the 50% blend between THIS stop and the next one falls, as a
  // fraction of the span between them. 0.5 is a straight lerp.
  //
  // Note which way round it is: `midpoint` is a POSITION, not a value. It says
  // "the 50 % blend lands here", so a midpoint of 0.73 pushes the blend late
  // and darkens a black-to-white ramp. This is the semantics of Photoshop's
  // diamond and of every editor that has one, and it is the opposite of the
  // guess ("the value at the middle") that the name invites.
  //
  // It is also the escape hatch §2 promised: linear-light interpolation puts a
  // black-to-white ramp's perceptual middle grey at linear 0.214041, and a
  // midpoint of 0.7322 on that segment moves it to t = 0.5 (measured: 0.21408
  // against a target of 0.214041).
  //
  // Implemented as an exponent, `t' = t^(ln 0.5 / ln midpoint)`, which is the
  // one-parameter family that maps [0,1] onto itself monotonically, fixes both
  // endpoints, and sends `midpoint` to exactly 0.5. Values outside (0, 1) are
  // clamped away from the endpoints, since 0 and 1 both send the exponent to
  // infinity and the segment to a step.
  float midpoint = 0.5f;
};

// One opacity stop, positioned independently of every colour stop -- §1.
struct OpacityStop {
  float position = 0.0f;
  // 0 = fully transparent, 1 = fully opaque. Clamped to [0, 1] at evaluation,
  // unlike colour: alpha above 1 is not "extra HDR coverage", it is a
  // malformed premultiplied texel that core/Blend's `over` would then read as
  // negative backdrop contribution.
  float opacity = 1.0f;
  // Same skew as ColorStop::midpoint, on the opacity ramp's own segments.
  float midpoint = 0.5f;
};

// The ramp: two independent lists.
//
// **Both lists must be sorted ascending by `position`** -- the caller's
// contract, exactly as `ops/PointOps`' `Curve` requires of its control points,
// and for the same reason: the sorted order is the editor's own stop order,
// and re-sorting defensively on every evaluation would cost an allocation per
// call in the inner loop of a full-canvas fill. `sortGradientStops()` below is
// what an editor calls after a drag reorders them.
//
// **The two empty cases are deliberately asymmetric, and this is a
// default-direction trap of the same family as core/SelectionMask.hpp's:**
//
//   No colour stops   -> the gradient has no colour, so it renders NOTHING.
//                        `renderGradient()` writes no texels and returns 0.
//   No opacity stops  -> FULLY OPAQUE everywhere. Not fully transparent.
//
// A gradient with an unauthored opacity ramp is the overwhelmingly common
// case (every two-colour gradient anyone has ever dragged), and it is opaque.
// Reading the absence as transparency would make the default gradient
// invisible, which is the same shape of bug as reading an absent selection as
// "select nothing".
struct GradientStops {
  std::vector<ColorStop> colorStops;
  std::vector<OpacityStop> opacityStops;
};

// Stable-sorts both lists ascending by position. For an editor to call after a
// drag; nothing in the render path calls it (see the contract above).
void sortGradientStops(GradientStops& stops);

// --------------------------------------------------------------------------
// Evaluation -- pure functions, no tiles, no selection
// --------------------------------------------------------------------------
//
// Split out from the render loop for the same reason `ops/PointOps` splits
// `applyLevelsChannel` out of `applyLevels`: each is independently
// hand-checkable, and a future GPU port or an editor's ramp preview wants the
// parameter math without the tile store.

// The ramp parameter at a document position, with `spread` already applied --
// so the result is in [0, 1] for `Repeat`/`Reflect`/`Angular`, and in [0, 1]
// after clamping for `Pad`.
//
// `px`/`py` are continuous document coordinates, NOT texel indices. The render
// loop samples at texel CENTRES, i.e. `(x + 0.5, y + 0.5)` for texel
// `(x, y)` -- the same convention `ops/Resample` uses, and the reason a
// gradient from (0,0) to (100,0) reads 0.005 at texel 0 rather than exactly 0.
//
// Degenerate geometry (p0 == p1) has no direction and no length, so there is
// no honest answer; it returns 0 for every point, which renders a flat fill of
// the first stop. Rejected: returning NaN or refusing to render, both of which
// turn a zero-length drag -- a single click, which every tool receives by
// accident -- into either a poisoned tile or an error dialog.
float gradientParameterAt(const GradientGeometry& geometry, float px, float py) noexcept;

// The colour ramp at `t`: STRAIGHT linear RGB, interpolated in linear light
// with each segment's midpoint skew applied.
//
// Outside the stop range this extrapolates FLAT (the nearest end stop's
// colour), matching `ops/PointOps`' `evalCurve` boundary rule and every
// gradient tool's `Pad`. Note that `gradientParameterAt` has usually already
// applied the spread, so this only sees out-of-range `t` when the stop
// positions themselves do not span [0, 1].
//
// Zero stops returns black -- but `renderGradient()` never asks, because zero
// colour stops means it renders nothing at all (see `GradientStops`).
std::array<float, 3> gradientColorAt(const GradientStops& stops, float t) noexcept;

// The opacity ramp at `t`, in [0, 1]. **Zero opacity stops returns 1.0**, per
// the asymmetry documented on `GradientStops`.
float gradientOpacityAt(const GradientStops& stops, float t) noexcept;

// The two ramps together as one straight (NOT premultiplied) RGBA sample:
// `{r, g, b, opacity}`. The premultiply happens in `renderGradient()` and
// nowhere else -- §2.
std::array<float, 4> gradientSampleStraight(const GradientStops& stops, float t) noexcept;

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
