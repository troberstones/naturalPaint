#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/Path.hpp"
#include "core/PathFlatten.hpp"

// core/PathRaster -- antialiased coverage for a filled path.
//
// ==========================================================================
// 1. The output is a callback, and that is most of the memory budget
// ==========================================================================
//
// This rasteriser never allocates a destination. It calls `SpanFn` with a run
// of per-texel coverage and forgets it. PRD A1 caps resident memory at 80 MB
// with no document open, and the four consumers of this file want four
// different destinations:
//
//   * PRD J2, path -> selection: 8-bit coverage into a `SelectionTile`.
//   * A Vector layer's fill: linear premultiplied rgba16float into an RGB tile.
//   * `clipPath` / `mask` (io/SvgImport): multiply into an existing coverage.
//   * A text glyph (text/CoreTextShaper): the same as the fill, at a smaller
//     scale.
//
// An intermediate buffer would serve all four badly and cost the largest of
// them. The only persistent allocation here is two scanline accumulators and
// one coverage row, each `clipWidth` floats, reused across rows and across
// calls through `PathRasterScratch` -- so the cost is O(width), not O(area),
// and a caller that rasterises a thousand glyphs allocates once.
//
// ==========================================================================
// 2. Cells, not samples: where this is exact and where it is not
// ==========================================================================
//
// The algorithm is the cell-accumulation scanline rasteriser used by
// FreeType's `ftgrays`, AGG, libart and `stb_truetype`'s v2 rasteriser. Each
// edge deposits, into every texel cell it crosses, two numbers: `cover` (the
// signed vertical extent it spans in that cell) and `area` (the signed area it
// cuts out of that cell, to the right of the edge). Sweeping a scanline left
// to right and accumulating `cover` gives the winding at every point; adding
// the local `area` gives the partial coverage of the crossed texel.
//
// **Be precise about the accuracy claim, because it is easy to overstate.**
// This is *exact* wherever the winding number is constant across a texel --
// which is every texel of a simple shape, including every texel of a glyph
// contour and of any path whose subpaths do not overlap. It is an
// *approximation* only in a texel where two edges of different contours cross
// each other, because there the fill rule has to be applied to a coverage
// value that has already been summed rather than to the winding at each point
// separately. Every rasteriser named above makes the same trade; the error is
// confined to the texels containing a crossing and is invisible in practice.
// What it is *not* is a supersampler: coverage is analytic area, so a
// near-horizontal edge steps in 1/255ths rather than in visible bands. That is
// core/SelectionShapes' argument for exact area, and this file is bound by it.
//
// **Both fill rules are first class**, and that constrains the algorithm
// rather than being a flag on it. The simpler signed-area prefix-sum
// rasteriser (font-rs, and much folklore) cannot express even-odd at all: it
// saturates the accumulated winding, so a self-overlapping contour fills
// solid. core/Path.hpp's `FillRule` explains why both are required -- glyph
// outlines and SVG's `fill-rule` both need even-odd -- so that approach was
// not available. The cell method applies the rule during the sweep, where the
// running winding is still a number rather than a clamped coverage.
//
// ==========================================================================
// 3. Clipping is a rectangle, and an edge outside it still counts
// ==========================================================================
//
// The caller gives a texel rectangle; nothing outside it is ever emitted. But
// an edge to the *left* of the rectangle still determines whether texels
// inside it are filled, so such edges are clamped into the first column rather
// than discarded. Dropping them is the classic bug where a shape larger than
// the tile being rendered comes out empty.
namespace np {

// One run of coverage on one scanline. `coverage` has `x1 - x0` entries, each
// in [0, 1], and is only valid for the duration of the call.
//
// Runs are emitted left to right, at most once per texel, and never with a
// coverage of zero -- a consumer can therefore treat "not called" as "not
// covered" and skip allocating a tile, which is what makes a sparse
// destination cheap.
using SpanFn = std::function<void(int32_t y, int32_t x0, int32_t x1,
                                  const float* coverage)>;

// The reusable row accumulators from section 1. Construct one and keep it
// alive across calls; it grows to the widest clip it has seen and never
// shrinks. Passing a fresh one per call is correct but allocates per call.
struct PathRasterScratch {
  std::vector<float> cover;
  std::vector<float> area;
  std::vector<float> coverage;
};

// Texel rectangle, half-open: x0 <= x < x1, y0 <= y < y1.
struct RasterClip {
  int32_t x0 = 0;
  int32_t y0 = 0;
  int32_t x1 = 0;
  int32_t y1 = 0;
};

// Rasterise already-flattened contours. This is the primitive; the two
// convenience overloads below flatten for you.
//
// Contours with fewer than two points, and non-finite coordinates, are
// skipped rather than refused -- core/PathFlatten has already applied
// `pathIsFinite()` at the untrusted-input boundary, and a second refusal here
// would give the caller two different ways to be told the same thing.
void rasterizeContours(const std::vector<FlatContour>& contours, FillRule rule,
                       const RasterClip& clip, PathRasterScratch& scratch,
                       const SpanFn& emit);

// Flatten and rasterise. `tolerancePx` is core/PathFlatten's, in the same
// space as the path's coordinates.
void rasterizePath(const Path& path, float tolerancePx, const RasterClip& clip,
                   PathRasterScratch& scratch, const SpanFn& emit);

// The clip that exactly contains a path's tight bounds, intersected with a
// document of `width` x `height`. Empty (x0 >= x1) when the path falls wholly
// outside, which callers use to skip the work entirely.
RasterClip clipForPath(const Path& path, int32_t width, int32_t height) noexcept;

}  // namespace np
