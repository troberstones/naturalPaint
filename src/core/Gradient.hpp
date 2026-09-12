#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// core/Gradient -- the gradient MODEL: a ramp, where it is, and how to
// evaluate it at a point. No tiles, no selection, no destination.
//
// ==========================================================================
// 0. Why this is in core/ and ops/Gradient.hpp is not
// ==========================================================================
//
// These types were all declared in ops/Gradient.hpp, which is the right home
// for `renderGradient()` -- a tile-store op -- and the wrong home for a type
// a `core::Document` has to hold. ops/Gradient.hpp includes core/TileStore.hpp
// and core/SelectionMask.hpp, and `core/VectorShape.hpp` says exactly why that
// mattered: a `VectorShape` is reachable from `core::Layer`, so anything it
// includes is included by nearly every translation unit in the build.
//
// So the model split out and the op stayed. ops/Gradient.hpp includes this
// header and re-exports nothing -- every existing name is still spelled
// `np::GradientStops`, `np::gradientColorAt` and so on, and every existing
// include of ops/Gradient.hpp still compiles. **There is exactly one gradient
// ramp type in this build**, which is the property that keeps a gradient drawn
// with the Gradient tool and a gradient filling a vector shape from
// interpolating differently.
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
// premultiply happens exactly once, at the point a texel is written --
// ops/Gradient's `renderGradient()` and core/VectorRaster's `paintCoverage()`
// are the two, and both do it after the colour and opacity ramps have each
// been evaluated. Interpolating premultiplied stop values instead gives a
// different answer through any fade, and a worse one:
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
//                        `renderGradient()` writes no texels and returns 0,
//                        and core/VectorRaster paints no texel either.
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

// --------------------------------------------------------------------------
// The document-level table
// --------------------------------------------------------------------------

// One entry of `core::Document::gradients`: a ramp, where it sits in DOCUMENT
// TEXEL coordinates, and a name for a panel to show.
//
// **Why a document table and not a gradient inside every `Paint`.** Two
// reasons, and the second is the one that decides it:
//
//  * Weight. `GradientStops` is two heap vectors. A `Paint` is a member of
//    `VectorShape`, which is a member of `core::Layer`, which `core::History`
//    snapshots BY VALUE on every edit -- so an inline ramp would be copied per
//    shape per undo step. At the table it is copied once per document per undo
//    step, and a document's table is a handful of entries however many
//    thousand shapes reference them.
//  * Sharing is the authored fact. An SVG file's `fill="url(#g)"` on forty
//    shapes is ONE gradient that forty shapes reference; edit it and all forty
//    change. Inlining copies would turn one edit into forty, and there would
//    then be no way to get back to "these shapes share a gradient" -- the
//    information is destroyed at import, not merely denormalised.
//    io/SvgImport.hpp section 3 asks for exactly this shape.
//
// **Lifetime: entries are never removed, and that is deliberate.** A
// `Paint::gradient` is a POSITION in this vector, so erasing an entry would
// renumber every entry after it and silently re-aim every shape that pointed
// past it -- the class of bug `Document::nextLayerId` exists to prevent one
// level up. Deleting the last shape that references a gradient therefore
// leaves the entry behind, at a cost of a few dozen bytes, and undoing that
// delete gets the shape's fill back intact. The table travels inside
// `core::Document`, so `core::History` restores it with the shapes that
// reference it, and a "compact unreferenced gradients" pass -- if anything
// ever wants one -- has to be an explicit edit that rewrites every index in
// the document at the same time, not a garbage collection.
struct GradientDef {
  GradientGeometry geometry;
  GradientStops stops;

  // What a panel shows, and what an importer carries across: an SVG
  // `<linearGradient id="...">`'s id, or a PSD `GdFl`'s `Grad/Nm  `. Empty is
  // normal and means "unnamed", exactly as `VectorShape::name` does.
  std::string name;
};

// `Document::gradients`' type, named so that the three signatures that thread
// it (`vectorContentHash`, `rasterizeVectorLayer`, io/GradientSerial) say the
// same thing.
using GradientTable = std::vector<GradientDef>;

// --------------------------------------------------------------------------
// Evaluation -- pure functions, no tiles, no selection
// --------------------------------------------------------------------------
//
// Split out from the render loop for the same reason `ops/PointOps` splits
// `applyLevelsChannel` out of `applyLevels`: each is independently
// hand-checkable, and a future GPU port or an editor's ramp preview wants the
// parameter math without the tile store.

// Stable-sorts both lists ascending by position. For an editor to call after a
// drag; nothing in the render path calls it (see the contract above).
void sortGradientStops(GradientStops& stops);

// The ramp played backwards: `t` in the result reads what `1 - t` read before.
//
// Both lists are reversed and every position becomes `1 - position`, so the
// sorted-ascending contract above still holds. **The midpoints move by one**,
// which is the part that is easy to get wrong and the reason this is a
// function rather than three lines at each call site: a midpoint belongs to
// the SEGMENT after its stop, so reversing sends segment `i` of `n-1` to
// segment `n-2-i`, and a segment whose 50 % blend landed at `m` now has it at
// `1 - m`. The final stop of each reversed list has no segment after it and
// keeps the default.
//
// **What this is NOT: an exact mirror of the ramp, and that is a property of
// the skew rather than of this function.** `ColorStop::midpoint` is
// implemented as `t^(ln 0.5 / ln m)`, and that family is not symmetric:
// `skew(1-x, 1-m)` is not `1 - skew(x, m)` in general (measured on m = 0.25,
// x = 0.75: 0.0354 against 0.134). So a reversed ramp agrees with the original
// at every STOP position and at every segment's own 50 %-blend position -- the
// two places the skew is pinned -- and differs by up to a tenth of a segment
// in between, for any segment whose midpoint is not 0.5. Reversing twice is
// still exactly the identity. This is what Photoshop's own Reverse does too if
// it uses the same one-parameter family, and closing the gap would mean a
// different skew, not a different reversal.
//
// Photoshop's `GdFl` "Reverse" checkbox is what this exists for (io/PsdVectorStyle),
// and it is a model operation rather than an importer one because a gradient
// editor wants the identical button.
void reverseGradientStops(GradientStops& stops);

// The ramp parameter at a document position, with `spread` already applied --
// so the result is in [0, 1] for `Repeat`/`Reflect`/`Angular`, and in [0, 1]
// after clamping for `Pad`.
//
// `px`/`py` are continuous document coordinates, NOT texel indices. Both
// render loops sample at texel CENTRES, i.e. `(x + 0.5, y + 0.5)` for texel
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
// Zero stops returns black -- but no render path ever asks, because zero
// colour stops means nothing is painted at all (see `GradientStops`).
std::array<float, 3> gradientColorAt(const GradientStops& stops, float t) noexcept;

// The opacity ramp at `t`, in [0, 1]. **Zero opacity stops returns 1.0**, per
// the asymmetry documented on `GradientStops`.
float gradientOpacityAt(const GradientStops& stops, float t) noexcept;

// The two ramps together as one straight (NOT premultiplied) RGBA sample:
// `{r, g, b, opacity}`. The premultiply happens at the write and nowhere
// else -- §2.
std::array<float, 4> gradientSampleStraight(const GradientStops& stops, float t) noexcept;

}  // namespace np
