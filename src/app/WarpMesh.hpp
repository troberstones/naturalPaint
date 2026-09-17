#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/SelectionMask.hpp"
#include "core/TileStore.hpp"
#include "ops/DocumentTransform.hpp"
#include "ops/Transform.hpp"

// app/WarpMesh (PRD D23).
//
// A Photoshop-style lattice Warp of a Free Transform target: an N x N
// Catmull-Rom spline NET over a rectangle in document space, dragged by its
// own grid points -- and ONLY those points. Pure and headless, like
// app/TransformSession.hpp, so `cancel()` stays free by construction.
//
// **Replaces a prior bicubic-Bezier design** (see git history before this
// change) whose net was `(3N+1) x (3N+1)` points -- 16 anchors plus 84
// tangent/twist handles for the "3x3" choice alone. A Catmull-Rom net is
// `N x N`, every point IS a visible, draggable anchor, and there is nothing
// else: "3x3" now means exactly 9 total control points, matching what the
// grid-size picker's own label has always claimed.
//
// Why this eliminates the old handle-sync class of bug, not just the handle
// COUNT: a bicubic Bezier patch's tangent handles are independent degrees of
// freedom that two neighbouring cells must agree on by construction (their
// shared edge's tangent is stored twice, once per patch) -- `dragControl()`'s
// old cardinal-handle ride-along was exactly that agreement being maintained
// by hand. A Catmull-Rom segment's tangent at grid point P[i] is instead
// DERIVED, every time, from `(P[i+1] - P[i-1]) / 2` -- the same two
// neighbours for every curve that touches P[i], with nothing stored
// separately per adjacent cell. Two cells sharing an edge therefore always
// agree on that edge's tangent, structurally, because they compute it from
// the identical neighbouring points rather than from two independently
// dragged handles. There is no synchronisation step to get right or forget.
//
// The cost of that: unlike a Bezier patch, a Catmull-Rom curve is not
// confined to the convex hull of its control points -- interpolating
// exactly through every point, with a neighbour-derived tangent, can overshoot
// past a neighbouring point's own position on a sharp local bend (the
// standard, well-documented Catmull-Rom "overshoot", the same tradeoff
// Maya/Houdini lattice deformers accept). Chosen deliberately over the old
// model's hull-bounded but handle-heavy alternative -- see the selftest's own
// case on this (a bend now visibly bulges past its neighbours' box, replacing
// the old convex-hull-containment assertion, which no longer holds).
//
// Grid boundary: `evaluate()` needs one phantom point on each side beyond
// the real net for the standard 4-point Catmull-Rom stencil to reach an edge
// segment. `phantom()` below reflects linearly (`P[-1] = P[0] - (P[1]-P[0])`)
// rather than clamping the tangent to zero at the edge -- reflection keeps
// the edge curve's tangent matching what an infinite net would give were it
// extended in a straight line, so dragging a corner bends the edge smoothly
// into it instead of flattening the tangent to a kink right at the boundary.
//
// `warpChordSubdivisions()`'s bound: the same per-cell chord-error argument
// the Bezier header used, adapted to the Catmull-Rom basis's own second-
// derivative bound (see the .cpp) -- `n > sqrt(3D / (4*tolerance))`, `D`
// bounded from each cell's own 4x4 point stencil, capped at
// `kMaxSubdivisionsPerCell` against a pathological drag.
//
// `warpImage()` rasterises hard-edged (no boundary antialiasing), matching
// what `ops/Transform.hpp` already does for the affine path, not a stronger
// guarantee this file invents. Refused, by name, in app/TransformSession:
// Pigment layers and layer masks (need the affine bridge's mass-weighted /
// hide-space packing re-run through a non-linear map -- unbuilt), and a
// LayerSet target (no per-member preview story either).
namespace np {

// The three grid choices the options row offers (PRD D23: "4x4 by default,
// with a 3x3 / 4x4 / 5x5 choice"). The value IS the number of control points
// per side now -- Grid3x3 really is 3x3 = 9 total points.
enum class WarpGridSize : int { Grid3x3 = 3, Grid4x4 = 4, Grid5x5 = 5 };

// A conservative ceiling on `warpChordSubdivisions()`'s answer, against a
// pathological drag forcing an unbounded tessellation.
inline constexpr int kMaxSubdivisionsPerCell = 48;

// One control point of the net: `row`/`col` index into an N x N array,
// `valid` false for "no control point" (an out-of-range hit test). Every
// valid ref is a plain, draggable grid point -- there is no anchor/handle
// distinction under Catmull-Rom, unlike the Bezier design this replaced.
struct WarpControlRef {
  bool valid = false;
  int row = 0;
  int col = 0;

  friend bool operator==(const WarpControlRef&, const WarpControlRef&) = default;
};

// The N x N Catmull-Rom spline control net over a rectangle in document
// space.
class WarpMesh {
 public:
  WarpMesh() = default;

  // A flat, unbent net over `bounds`. `n` clamped to `[3, 5]`.
  static WarpMesh flat(const DocumentRegion& bounds, int n);

  // Number of control points per side (3, 4, or 5) -- also the grid's own
  // "NxN" label, unlike the old Bezier net where `n()` counted CELLS and
  // `pointsPerSide()` was `3*n()+1`.
  int n() const noexcept { return n_; }
  int pointsPerSide() const noexcept { return n_; }
  // Number of spline cells per side: one fewer than the point count.
  int cells() const noexcept { return n_ - 1; }
  const DocumentRegion& bounds() const noexcept { return bounds_; }

  Point2 at(int row, int col) const noexcept;
  void setAt(int row, int col, Point2 p) noexcept;

  // True when every control point lies within `toleranceDoc` of a single
  // affine fit through three reference corners -- i.e. never bent off a
  // plane. Used by `refit()` and the identity-commit selftest case.
  bool isAffine(float toleranceDoc = 1e-2f) const noexcept;

  // Bit-exact equality against `flat(bounds(), n())`: nothing has been
  // dragged since. `warpImage()`'s fast path uses this like `ops/Transform
  // .hpp`'s `exactRemapKind()` -- an untouched net should reproduce the
  // source, not resample a near-no-op through a kernel.
  bool isIdentity() const noexcept;

  // Moves control point `ref` by `delta`. A no-op for an invalid `ref`. No
  // neighbour ever rides along: under Catmull-Rom every OTHER point's own
  // position is unaffected by this one moving (only the DERIVED tangents
  // through this point's still-stationary neighbours change), so there is
  // nothing left to translate the way the old Bezier `dragControl()` rode
  // an anchor's cardinal handles along with it.
  void dragControl(WarpControlRef ref, Point2 delta) noexcept;

  // A net of `newN` points per side describing the same shape. Exact when
  // `isAffine()` (re-mapped through the same affine fit). Otherwise the new
  // net's points are placed at this surface's own `evaluate()` -- a bent
  // Catmull-Rom surface at a different N cannot in general reproduce every
  // point (a different N changes every tangent's own neighbour spacing), but
  // every new point's POSITION still lands exactly on the old surface.
  WarpMesh refit(int newN) const;

  // Evaluates the surface at (u, v) in `[0, cells()] x [0, cells()]` --
  // (0,0) is the net's top-left point, (cells(), cells()) its bottom-right.
  // Clamps `u`/`v` into range first, so a caller need not clamp itself.
  Point2 evaluate(float u, float v) const noexcept;

  // The one real control point at (row, col), or a reflected phantom for a
  // one-past-the-edge index -- the boundary rule the header documents.
  // `evaluate()`'s own 4-point stencils never reach more than one step
  // beyond the real net, so `row`/`col` here are only ever in
  // `[-1, pointsPerSide()]`. Public because `warpChordSubdivisions()` (a
  // free function) needs the same boundary-extended points `evaluate()`
  // does, to bound curvature at an edge cell honestly.
  Point2 extendedAt(int row, int col) const noexcept;

 private:
  int n_ = 4;
  DocumentRegion bounds_;
  std::vector<Point2> net_;  // n_*n_, row-major: net_[row*n_+col]
};

// Nearest control point to `cursor` within `radius` (any consistent unit --
// document pixels or screen pixels divided back to document space by the
// caller, matching `app/TransformSession.hpp`'s own convention for its
// affine `hitTestTransformHandle()`). Returns an invalid ref when nothing is
// within radius.
WarpControlRef hitTestWarpControl(const WarpMesh& mesh, Point2 cursor, float radius) noexcept;

// The header's chord-error derivation, as code. `toleranceDoc` is the max
// allowed chord deviation, in document pixels.
int warpChordSubdivisions(const WarpMesh& mesh, float toleranceDoc = 0.5f) noexcept;

// One tessellated cell: four DOCUMENT-space corners (evaluated from the
// surface) and the matching (u, v) rectangle they were evaluated at, in the
// mesh's own `[0, cells()] x [0, cells()]` parameter space.
struct WarpQuad {
  Point2 dst00, dst10, dst11, dst01;  // (u0,v0) (u1,v0) (u1,v1) (u0,v1)
  float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};

// Every cell sliced into `subdivisionsPerCell` x `subdivisionsPerCell` quads
// -- `cells() * cells() * subdivisionsPerCell^2` of them in total.
std::vector<WarpQuad> tessellateWarpMesh(const WarpMesh& mesh, int subdivisionsPerCell);

// The destination footprint: the smallest integer region containing every
// tessellated sample point, outset by one pixel -- the identical rounding
// margin `ops/DocumentTransform.hpp`'s `transformedRegion()` documents and
// for the identical reason (a texel is only ever written when its CENTRE
// maps inside the source, so the true written set is already inside this
// box).
DocumentRegion warpedRegion(const WarpMesh& mesh, int subdivisionsPerCell) noexcept;

// Forward-warps `src` (must cover exactly `mesh.bounds()`) into `*out`, sized
// to `dstRegion`. `*out` starts transparent black; only texels the warped
// mesh covers are written, so a caller compositing over prior content gets
// "outside untouched" for free. False, `*out` cleared, on a size mismatch, a
// null/aliased `out`, or an empty `dstRegion`.
bool warpImage(const TransformImage& src, const WarpMesh& mesh, const DocumentRegion& dstRegion,
              ResampleKernel kernel, TransformImage* out, std::string* errorOut);

// The tile-store bridge, `ops/DocumentTransform.hpp`'s `transformRgbTiles()`
// with a `WarpMesh` in place of a `Mat3`. `in` must cover at least
// `mesh.bounds()` -- only that rectangle is read. As with the affine
// bridge, a destination tile that comes out entirely transparent and did not
// already exist in `*out` is not allocated.
bool warpRgbTiles(const TileStore& in, const WarpMesh& mesh, const DocumentRegion& dstRegion,
                  ResampleKernel kernel, TileStore* out, std::string* errorOut);

// `ops::transformSelectionCoverage()`'s warp equivalent: coverage packed into
// a one-channel `TransformImage`, warped through `warpImage()`, unpacked back
// into a `Selection`. `srcRegion` must equal `mesh.bounds()`.
bool warpSelectionCoverage(const Selection& in, const DocumentRegion& srcRegion,
                          const WarpMesh& mesh, const DocumentRegion& dstRegion,
                          ResampleKernel kernel, Selection* out, std::string* errorOut);

}  // namespace np
