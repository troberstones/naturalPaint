#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/SelectionMask.hpp"
#include "core/TileStore.hpp"
#include "ops/DocumentTransform.hpp"
#include "ops/Transform.hpp"

// app/WarpMesh (PRD D23; track `warp` of the `reach` wave).
//
// A Photoshop-style lattice Warp of a Free Transform target: an N x N grid of
// bicubic Bezier patches over a rectangle, dragged by anchors and tangent
// handles. Pure and headless, like app/TransformSession.hpp, so `cancel()`
// stays free by construction.
//
// The net is (3N+1) x (3N+1) control points, row-major (`row` = V, `col` =
// U); cell (I,J) owns the 4x4 block at `row in [3J,3J+3], col in [3I,3I+3]`,
// evaluated with the Bernstein basis per axis. Anchors are `row%3==0 &&
// col%3==0`; every other point is a tangent handle. `dragControl()` moves an
// anchor's up-to-four cardinal handles with it (rigid translate, so tangents
// keep their length and direction) but not the four diagonal Coons "twist"
// points -- shared by four curves at once, and out of this track's brief
// ("drag anchors and their curve handles").
//
// `warpChordSubdivisions()`'s bound, one line: a cubic Bezier's chord error
// over `n` subdivisions is `<= 3D / (4n^2)` where `D` bounds `|P''|` (from
// the standard `|B''| <= 6D` second-derivative bound plus the standard
// `h^2/8 * max|B''|` chord-remainder bound), so `n > sqrt(3D / (4*tolerance))`
// -- extended to the whole surface by bounding each cell's own `D` from its
// stored control rows/columns (Bernstein's partition-of-unity), no evaluation
// needed. Capped at `kMaxSubdivisionsPerCell` against a pathological drag.
//
// `warpImage()` rasterises hard-edged (no boundary antialiasing), matching
// what `ops/Transform.hpp` already does for the affine path, not a stronger
// guarantee this file invents. Refused, by name, in app/TransformSession:
// Pigment layers and layer masks (need the affine bridge's mass-weighted /
// hide-space packing re-run through a non-linear map -- unbuilt), and a
// LayerSet target (no per-member preview story either).
namespace np {

// The three grid choices the options row offers (PRD D23: "4x4 by default,
// with a 3x3 / 4x4 / 5x5 choice").
enum class WarpGridSize : int { Grid3x3 = 3, Grid4x4 = 4, Grid5x5 = 5 };

// A conservative ceiling on `warpChordSubdivisions()`'s answer, against a
// pathological drag forcing an unbounded tessellation.
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
// document space.
class WarpMesh {
 public:
  WarpMesh() = default;

  // A flat, unbent net over `bounds`. `n` clamped to `[3, 5]`.
  static WarpMesh flat(const DocumentRegion& bounds, int n);

  int n() const noexcept { return n_; }
  int pointsPerSide() const noexcept { return 3 * n_ + 1; }
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

  // Moves control point `ref` by `delta`. A no-op for an invalid `ref`.
  void dragControl(WarpControlRef ref, Point2 delta) noexcept;

  // A net of `newN` cells describing the same shape. Exact when `isAffine()`
  // (re-mapped through the same affine fit). Otherwise only the new net's
  // ANCHORS are placed exactly, at `evaluate()` of this net; handles are
  // re-derived locally like `flat()` does -- a bent surface at a different N
  // cannot in general reproduce every interior point, only anchors.
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

// The header's chord-error derivation, as code. `toleranceDoc` is the max
// allowed chord deviation, in document pixels.
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
