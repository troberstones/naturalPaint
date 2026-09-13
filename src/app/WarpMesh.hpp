#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/TileStore.hpp"
#include "ops/DocumentTransform.hpp"
#include "ops/Transform.hpp"

// app/WarpMesh (PRD D23; track `warp` of the `reach` wave).
//
// A Photoshop-style lattice Warp of a Free Transform target: an N x N grid of
// bicubic Bezier patches over a rectangle, dragged by its anchors and their
// tangent handles. Pure and headless -- no ui/ dependency, matching
// app/TransformSession.hpp's own rule for the identical reason: `cancel()`
// must stay free, and the only way to make that a compile-time fact rather
// than a promise is to keep the model where a UI cannot reach into it.
//
// ==========================================================================
// (1) THE CONTROL NET
// ==========================================================================
//
// A grid of N x N cells is a (3N+1) x (3N+1) net of control points in
// DOCUMENT space, row-major, `row` walking the V axis and `col` walking U.
// Cell (I, J) (0 <= I, J < N) owns the 4x4 block of points at
// `row in [3J, 3J+3], col in [3I, 3I+3]` -- the standard tensor-product
// bicubic Bezier patch, evaluated with the Bernstein basis in each direction
// independently (`evaluate()` below).
//
// **Anchors** sit at `row % 3 == 0 && col % 3 == 0` -- the N+1 x N+1 grid
// intersections a user actually sees as draggable squares. **Handles** are
// every other control point: the tangent points that shape the curve between
// two anchors. `WarpControlRef::isAnchor()` is the one predicate that tells
// the two apart, and it is arithmetic on the index, not a stored flag.
//
// ==========================================================================
// (2) flat(): WHY AN EVENLY-SPACED NET REPRODUCES A RECTANGLE EXACTLY
// ==========================================================================
//
// A cubic Bezier curve representing a straight line from A to B has control
// points `A, A + (B-A)/3, A + 2(B-A)/3, B` -- the standard "flatten a Bezier
// to a line" construction, run backwards. Applied to every row and every
// column of the net at once, control point (row, col) lands at
//
//     x = bounds.x + (col / (3N)) * bounds.width
//     y = bounds.y + (row / (3N)) * bounds.height
//
// which is simply an evenly spaced sampling of the rectangle's own edges --
// every cell's four corners are its own quarter of the rectangle and every
// handle sits at the true one-third point of the corresponding straight
// edge. `evaluate()` therefore reproduces the rectangle's interior exactly
// (see this file's `isAffine()` and the selftest's "identity net commits
// bit-identical" case): a bicubic patch whose sixteen control points all lie
// on one plane is that plane, not merely close to it.
//
// ==========================================================================
// (3) DRAGGING: ANCHORS CARRY THEIR HANDLES, HANDLES MOVE ALONE
// ==========================================================================
//
// `dragControl()` moves the point named by `WarpControlRef` by `delta`. For
// an anchor, it moves the up-to-four CARDINAL neighbours (row +/- 1 same
// column, col +/- 1 same row) by the identical `delta` -- a rigid translate,
// which keeps every one of those tangent vectors exactly as it was, in both
// direction and length, satisfying "moves its adjacent handles with it"
// without inventing a rotation or a scale of the tangent nobody asked for.
//
// **Scope reduction, named**: the four DIAGONAL corners of a cell's 4x4
// block (row +/- 1 AND col +/- 1 from an anchor) are Coons-patch "twist"
// points, not any single anchor's tangent handle -- they are shared by four
// curves at once, and a faithful twist convention is a separate piece of
// surface theory this track's brief does not ask for ("drag anchors and
// their curve handles" -- cardinal handles only). They are left exactly
// where `flat()`/`refit()` put them, which keeps a dragged anchor's patch a
// well-behaved bicubic bulge; a true four-way twist vector is not built
// here.
//
// ==========================================================================
// (4) TESSELLATION AND THE CHORD-ERROR BOUND -- THE DERIVATION
// ==========================================================================
//
// `warpChordSubdivisions()` picks how finely to slice each cell so that no
// point of the true bicubic surface strays more than `toleranceDoc` document
// pixels from the nearest straight edge of the tessellated approximation.
//
//   (a) A cubic Bezier's second derivative is bounded by
//       `|B''(t)| <= 6 * D`, where `D = max(|P0-2P1+P2|, |P1-2P2+P3|)` --
//       the standard bound from differentiating the Bernstein form twice.
//
//   (b) Approximating a C2 curve by straight chords over sub-intervals of
//       width `h = 1/n` has a standard remainder bound of `(h^2 / 8) *
//       max|B''|` (the same bound linear interpolation's error carries
//       generally). Substituting (a): chord error `<= (6D) / (8 n^2) =
//       3D / (4 n^2)`.
//
//   (c) Solve `3D / (4 n^2) < tolerance` for `n`: `n > sqrt(3D / (4 *
//       tolerance))`.
//
//   (d) **Extending (a)-(c) from a curve to the whole tensor-product
//       surface, without evaluating it everywhere.** Fixing V at any value
//       and varying U gives a cubic Bezier in U whose control points are
//       `Q_i(V) = sum_j Bj(V) * P_ij` -- a BLEND of the cell's own control
//       ROWS, and the Bernstein basis `Bj` is a non-negative partition of
//       unity. So `|Q0 - 2Q1 + Q2| = |sum_j Bj(V) (P0j - 2P1j + P2j)| <=
//       sum_j Bj(V) * |P0j - 2P1j + P2j| <= max_j |P0j - 2P1j + P2j|` --
//       i.e. the curve's own `D` at ANY fixed V is bounded by the max second
//       difference of the cell's four control ROWS, taken directly from the
//       stored points with no evaluation at all. Symmetrically for V-
///      direction curves at any fixed U, bounded by the four control
//       COLUMNS. The global `D` for the whole net is the max of both, over
//       every cell.
//
// A single global subdivision count (rather than one per cell) is used for
// simplicity -- `tessellateWarpMesh()` slices every cell into the same N_sub
// x N_sub grid of quads -- which is conservative (a gently-bent cell pays
// for the worst cell's own bend) rather than optimal, and named as such.
// Capped at `kMaxSubdivisionsPerCell` so a pathological drag (control points
// forced apart by a very large delta) cannot make one commit tessellate an
// unbounded number of quads; past the cap the bound is no longer guaranteed
// and this is stated rather than silently exceeded.
//
// ==========================================================================
// (5) RENDERING: FORWARD-MAPPED QUADS, INVERSE-BILINEAR PER DESTINATION
//     TEXEL, SAMPLED WITH THE SESSION'S OWN KERNEL
// ==========================================================================
//
// `warpImage()` tessellates the mesh, and for each small destination quad
// (four DOCUMENT-space corners, and the matching source-space (u, v)
// rectangle those corners were evaluated from) rasterises every destination
// texel whose centre falls inside it: `invertBilinearQuad()` solves for the
// texel's normalised position (s, t) inside the quad by Newton iteration
// (the quad is nearly a parallelogram once tessellation is fine enough to
// meet the chord bound, so a few iterations from the centre converge
// reliably), then that same (s, t) locates a point in the source
// RECTANGLE'S (u, v) space -- treating the quad as bilinear over its own
// small parametric cell, which is the same approximation the chord bound
// already licenses (a cell fine enough to look straight is also fine enough
// to look bilinear across its own small span) -- and that point is sampled
// from the source image with `resampleKernelWeight()`/`resampleKernelRadius()`,
// clamped at the source's own edge exactly as `ops/Transform.hpp`'s
// `transformImage()` clamps kernel taps that overhang the crop.
//
// **Hard edge, not coverage-weighted -- matching what Free Transform
// actually does, not what this track's brief assumed it does.**
// `ops/Transform.hpp` section 5 names "boundary coverage antialiasing" as
// NOT implemented for the affine path: a rotated layer's silhouette is a
// hard, aliased edge there too. This file inherits the identical limitation
// rather than building a coverage-weighted edge Free Transform itself does
// not have -- a destination texel is either inside a tessellated quad (hard
// full-weight source read) or outside every one of them (left exactly as
// `*out` was, i.e. transparent, so a caller compositing over prior content
// gets "outside texels untouched" for free).
//
// ==========================================================================
// (6) WHAT IS NOT HERE
// ==========================================================================
//
//   - **Pigment layers and layer masks.** Warping either needs the identical
//     bridge `ops/DocumentTransform.hpp` already built for the affine path
//     (mass-weighted dual-image packing for pigment; hide-space packing for
//     a mask) run through THIS file's non-linear map instead of a `Mat3`.
//     That is real, separate machinery and this track's time did not extend
//     to building a second copy of it for a warp. `app/TransformSession`
//     refuses a warp of a Pigment layer or a layer carrying a mask, by name.
//   - **A LayerSet target.** Warping a whole set through one net falls out
//     of nothing already built (there is no per-member preview story for it
//     either, `app/TransformSession.hpp` section 8's own closing paragraph)
//     and is refused by name rather than half-built.
//   - **Moving the selection's coverage with a `SelectionPixels` warp.**
//     The affine path's `ops::transformSelectionCoverage()` has no warp
//     equivalent here -- it would need the identical generic image-warp this
//     file already has, just pointed at the selection's own tile store,
//     which is real but unbuilt work. `TransformSession::commit()` leaves
//     `od.selection` at its pre-warp shape; named in that file, not hidden.
namespace np {

// The three grid choices the options row offers (PRD D23: "4x4 by default,
// with a 3x3 / 4x4 / 5x5 choice").
enum class WarpGridSize : int { Grid3x3 = 3, Grid4x4 = 4, Grid5x5 = 5 };

// A conservative ceiling on `warpChordSubdivisions()`'s answer -- section 4's
// own note on why an unbounded tessellation is refused rather than paid for.
inline constexpr int kMaxSubdivisionsPerCell = 48;

// One control point of the net: `row`/`col` index into a (3N+1) x (3N+1)
// array, `valid` false for "no control point" (an out-of-range hit test).
struct WarpControlRef {
  bool valid = false;
  int row = 0;
  int col = 0;

  bool isAnchor() const noexcept { return valid && row % 3 == 0 && col % 3 == 0; }
  friend bool operator==(const WarpControlRef&, const WarpControlRef&) = default;
};

// The (3N+1) x (3N+1) bicubic Bezier control net over a rectangle in
// document space. See this header's sections 1-3.
class WarpMesh {
 public:
  WarpMesh() = default;

  // A flat, unbent net over `bounds` -- section 2. `n` is clamped to
  // `[3, 5]`, the only grid sizes the options row offers.
  static WarpMesh flat(const DocumentRegion& bounds, int n);

  int n() const noexcept { return n_; }
  int pointsPerSide() const noexcept { return 3 * n_ + 1; }
  const DocumentRegion& bounds() const noexcept { return bounds_; }

  Point2 at(int row, int col) const noexcept;
  void setAt(int row, int col, Point2 p) noexcept;

  // True when every control point still lies on a SINGLE affine map of the
  // net's own flat (u, v) fractions -- i.e. this net has never been bent
  // away from a plane. Fitted from three reference points
  // (`at(0,0)`/`at(0, 3n)`/`at(3n, 0)`) and checked against every other
  // point within `toleranceDoc` document pixels. `refit()` and the
  // identity-commit selftest case both use this.
  bool isAffine(float toleranceDoc = 1e-2f) const noexcept;

  // Bit-exact equality against `flat(bounds(), n())` -- i.e. "nobody has
  // dragged anything since this net was built or last baked from an
  // identity affine transform". `warpImage()`'s own fast path uses this the
  // identical way `ops/Transform.hpp` section 4 uses `exactRemapKind()`:
  // an untouched net should reproduce the source, not resample a copy of
  // it through a kernel that happens to be very close to a no-op.
  bool isIdentity() const noexcept;

  // Moves control point `ref` by `delta` -- section 3. A no-op for an
  // invalid `ref`.
  void dragControl(WarpControlRef ref, Point2 delta) noexcept;

  // A net of `newN` cells describing the SAME shape as this net, as closely
  // as that shape can be represented. **Exact when `isAffine()`** (the new
  // flat net's own fractional positions are re-mapped through the same
  // affine fit, so an unbent-but-moved net survives an N change losslessly).
  // Otherwise the new net's ANCHORS are placed at `evaluate()` of this net,
  // at the matching normalised position, and its handles are re-derived
  // locally the way `flat()` derives them -- an approximation, named as one
  // here rather than claimed exact: a bent surface re-expressed at a
  // different N cannot, in general, reproduce every interior point of the
  // original bicubic surface, only its anchor positions.
  WarpMesh refit(int newN) const;

  // Evaluates the surface at (u, v) in `[0, n] x [0, n]` -- (0,0) is the
  // net's top-left anchor, (n, n) its bottom-right. Clamps `u`/`v` into
  // range first, so a caller need not clamp itself.
  Point2 evaluate(float u, float v) const noexcept;

 private:
  int n_ = 4;
  DocumentRegion bounds_;
  std::vector<Point2> net_;  // (3n+1)*(3n+1), row-major: net_[row*(3n+1)+col]
};

// Nearest control point to `cursor` within `radius` (any consistent unit --
// document pixels or screen pixels divided back to document space by the
// caller, matching `app/TransformSession.hpp`'s own convention for its
// affine `hitTestTransformHandle()`). Returns an invalid ref when nothing is
// within radius.
WarpControlRef hitTestWarpControl(const WarpMesh& mesh, Point2 cursor, float radius) noexcept;

// Section 4's derivation, as code. `toleranceDoc` is the maximum allowed
// chord deviation, in document pixels.
int warpChordSubdivisions(const WarpMesh& mesh, float toleranceDoc = 0.5f) noexcept;

// One tessellated cell: four DOCUMENT-space corners (evaluated from the
// surface) and the matching (u, v) rectangle they were evaluated at, in the
// mesh's own `[0, n] x [0, n]` parameter space.
struct WarpQuad {
  Point2 dst00, dst10, dst11, dst01;  // (u0,v0) (u1,v0) (u1,v1) (u0,v1)
  float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};

// Every cell sliced into `subdivisionsPerCell` x `subdivisionsPerCell` quads
// -- `n() * n() * subdivisionsPerCell^2` of them in total.
std::vector<WarpQuad> tessellateWarpMesh(const WarpMesh& mesh, int subdivisionsPerCell);

// The destination footprint: the smallest integer region containing every
// tessellated sample point, outset by one pixel -- the identical rounding
// margin `ops/DocumentTransform.hpp`'s `transformedRegion()` documents and
// for the identical reason (a texel is only ever written when its CENTRE
// maps inside the source, so the true written set is already inside this
// box).
DocumentRegion warpedRegion(const WarpMesh& mesh, int subdivisionsPerCell) noexcept;

// Forward-warps `src` (which must cover exactly `mesh.bounds()`, i.e.
// `src.width == mesh.bounds().width` and likewise for height) into `*out`,
// sized to `dstRegion`. Section 5. `*out` is cleared to transparent black
// first and only texels the warped mesh actually covers are written -- see
// this header's own "outside texels untouched" note for what that buys a
// caller compositing the result over prior content.
//
// Returns false, leaving `*out` cleared, when `src`'s extent does not match
// `mesh.bounds()`, `out` is null or aliases `src`, or `dstRegion` is empty.
bool warpImage(const TransformImage& src, const WarpMesh& mesh, const DocumentRegion& dstRegion,
              ResampleKernel kernel, TransformImage* out, std::string* errorOut);

// The tile-store bridge, `ops/DocumentTransform.hpp`'s `transformRgbTiles()`
// with a `WarpMesh` in place of a `Mat3`. `in` must cover at least
// `mesh.bounds()` -- only that rectangle is read. As with the affine
// bridge, a destination tile that comes out entirely transparent and did not
// already exist in `*out` is not allocated.
bool warpRgbTiles(const TileStore& in, const WarpMesh& mesh, const DocumentRegion& dstRegion,
                  ResampleKernel kernel, TileStore* out, std::string* errorOut);

}  // namespace np
