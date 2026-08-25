#include "core/SelectionRefine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "color/Space.hpp"
#include "core/Premultiply.hpp"
#include "ops/PointOps.hpp"

namespace np {
namespace {

// --- The distance transform ------------------------------------------------

// "There is no seed in this column/row", carried as an ordinary finite number
// rather than as an infinity.
//
// The lower envelope below divides differences of `f[q] + q*q`, and with
// infinities that difference is NaN for any two non-seed columns, which sends
// the envelope's intersection abscissa to NaN and the whole pass with it. A
// large finite value keeps the arithmetic ordinary: two non-seed columns
// differ by exactly `q*q - v*v`, so they intersect at their midpoint, which is
// what the algorithm expects of two equal-height parabolas.
//
// The magnitude is chosen against **double's** mantissa, and that is why this
// transform is not run in float. The largest real squared distance a plane
// here can produce is under 1e9 (a 4096-texel side plus apron, squared, twice),
// so the sentinel must dominate that; and `kFar + q*q` must still *record*
// `q*q`, so the sentinel's ULP must stay well under 1. At 1e12 double's ULP is
// 1.2e-4 -- three orders of margin. In float the ULP at 1e12 is 65536 and
// every non-seed column would collapse to the same height, so the midpoint
// structure above would be destroyed and the argmin below would be arbitrary.
constexpr double kFar = 1e12;

// How far around the feature transform's answer the true cone minimum is
// searched. Derived from a measured sweep; the derivation sits at the use site,
// next to the argument for why a search is needed at all.
constexpr int32_t kRefineWindow = 3;

// Felzenszwalb & Huttenlocher's 1-D lower envelope of parabolas, **with the
// argmin**.
//
// `d[q] = min_p (f[p] + (q - p)^2)`, and `arg[q]` is the `p` that achieved it.
// The argmin is not an extra pass: `v[k]` is already the index of the parabola
// owning the envelope over `[z[k], z[k+1])`, so the feature transform costs one
// store. §2 of the header needs it -- the sub-texel offset lives on the seed,
// and without knowing *which* seed won there is nothing to add it to.
//
// `v` and `z` are the caller's scratch (n and n+1 entries) so that a 4096-row
// pass does not allocate 4096 times.
void envelope1d(const double* f, int32_t n, double* d, int32_t* arg, int32_t* v, double* z) {
  const double inf = std::numeric_limits<double>::infinity();
  int32_t k = 0;
  v[0] = 0;
  z[0] = -inf;
  z[1] = inf;
  const auto cross = [&](int32_t q, int32_t p) {
    return ((f[q] + static_cast<double>(q) * q) - (f[p] + static_cast<double>(p) * p)) /
           (2.0 * static_cast<double>(q) - 2.0 * static_cast<double>(p));
  };
  for (int32_t q = 1; q < n; ++q) {
    double s = cross(q, v[k]);
    // Terminates at k == 0 without a bounds test because z[0] is -infinity and
    // `s` is finite for finite `f` -- which is the second reason kFar above is
    // not an infinity. A NaN `s` here would run k off the bottom of the array.
    while (s <= z[k]) {
      --k;
      s = cross(q, v[k]);
    }
    ++k;
    v[k] = q;
    z[k] = s;
    z[k + 1] = inf;
  }
  k = 0;
  for (int32_t q = 0; q < n; ++q) {
    while (z[k + 1] < q) ++k;
    const double dx = static_cast<double>(q) - static_cast<double>(v[k]);
    d[q] = dx * dx + f[v[k]];
    arg[q] = v[k];
  }
}

// The exact 2-D Euclidean distance transform of a seed set, plus the feature
// transform: for every texel, the squared distance to the nearest seed and that
// seed's coordinates.
//
// Separable in two 1-D passes because the *squared* Euclidean distance is a sum
// over axes -- which is the whole reason the parabola form is the one that
// works and the cone form (`min_q |p-q| + offset(q)`, the field §2 actually
// wants) is not separable at all. The feature is reassembled the standard way:
// the row pass names the winning column `x*`, and the column pass at `x*`
// already named the winning row within it.
struct FeatureField {
  std::vector<double> squared;
  std::vector<int32_t> seedX;
  std::vector<int32_t> seedY;
};

FeatureField euclideanFeatureTransform(const std::vector<double>& seedCost, int32_t w,
                                       int32_t h) {
  FeatureField out;
  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
  out.squared.assign(n, 0.0);
  out.seedX.assign(n, 0);
  out.seedY.assign(n, 0);

  const int32_t longest = std::max(w, h);
  std::vector<double> f(static_cast<size_t>(longest));
  std::vector<double> d(static_cast<size_t>(longest));
  std::vector<int32_t> arg(static_cast<size_t>(longest));
  std::vector<int32_t> v(static_cast<size_t>(longest));
  std::vector<double> z(static_cast<size_t>(longest) + 1);

  // Pass 1, down each column: the nearest seed **within that column**.
  std::vector<double> colDist(n);
  std::vector<int32_t> colArg(n);
  for (int32_t x = 0; x < w; ++x) {
    for (int32_t y = 0; y < h; ++y) f[static_cast<size_t>(y)] = seedCost[static_cast<size_t>(y) * w + x];
    envelope1d(f.data(), h, d.data(), arg.data(), v.data(), z.data());
    for (int32_t y = 0; y < h; ++y) {
      colDist[static_cast<size_t>(y) * w + x] = d[static_cast<size_t>(y)];
      colArg[static_cast<size_t>(y) * w + x] = arg[static_cast<size_t>(y)];
    }
  }

  // Pass 2, across each row, over pass 1's answers rather than over the seeds.
  // This is the step that makes the result exact rather than a per-axis
  // approximation: the parabola raised to height `colDist` encodes "the best
  // this column can offer at any horizontal distance".
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) f[static_cast<size_t>(x)] = colDist[static_cast<size_t>(y) * w + x];
    envelope1d(f.data(), w, d.data(), arg.data(), v.data(), z.data());
    for (int32_t x = 0; x < w; ++x) {
      const size_t i = static_cast<size_t>(y) * w + x;
      out.squared[i] = d[static_cast<size_t>(x)];
      const int32_t sx = arg[static_cast<size_t>(x)];
      out.seedX[i] = sx;
      out.seedY[i] = colArg[static_cast<size_t>(y) * w + sx];
    }
  }
  return out;
}

// --- The whole-document predicate pass (PRD E9) ----------------------------

// One pass over the document, one coverage answer per texel, no connectivity.
//
// Shared by colour range and luminance range because the *walk* is identical
// and only the predicate differs; the alternative is two copies of the
// edge-tile clip below, which is the thing most likely to be got wrong twice.
//
// `emptyMatches` decides the tile set, and it is the same decision
// `ops/FloodFill`'s `globalSimilar()` makes: normally only the source's
// occupied tiles can hold a match, but if the implicit {0,0,0,0} texel is in
// range then so is every texel no tile was ever allocated for, and the answer
// really is every tile in the document. Charged, not truncated.
template <typename CoverageFn>
Selection documentPredicatePass(const TileStore& source, int32_t width, int32_t height,
                                bool emptyMatches, CoverageFn coverageOf) {
  Selection out;
  if (width <= 0 || height <= 0) return out;

  std::vector<TileCoord> coords;
  if (emptyMatches) {
    const TileCoord last = tileCoordAt(PixelCoord{width - 1, height - 1});
    for (int32_t ty = 0; ty <= last.y; ++ty) {
      for (int32_t tx = 0; tx <= last.x; ++tx) coords.push_back(TileCoord{tx, ty});
    }
  } else {
    for (const auto& [coord, tile] : source) {
      (void)tile;
      coords.push_back(coord);
    }
  }

  for (const TileCoord coord : coords) {
    const Tile* src = source.find(coord);
    const PixelCoord origin = tileOrigin(coord);
    SelectionTile built;
    bool any = false;
    for (int32_t ly = 0; ly < kTileSize; ++ly) {
      const int32_t docY = origin.y + ly;
      // Clipped to the DOCUMENT, not to the tile. An edge tile of a 100-tall
      // document holds 128 rows and 28 of them are not part of the picture;
      // selecting them would hand every downstream consumer coverage over
      // texels that do not exist.
      if (docY < 0 || docY >= height) continue;
      for (int32_t lx = 0; lx < kTileSize; ++lx) {
        const int32_t docX = origin.x + lx;
        if (docX < 0 || docX >= width) continue;
        const PixelCoord local{lx, ly};
        const std::array<float, 4> texel =
            src != nullptr ? src->readPixel(local) : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
        const float coverage = coverageOf(texel);
        if (coverage <= 0.0f) continue;
        built.writeCoverage(local, coverage);
        any = true;
      }
    }
    // The constructor invariant: an entirely-zero tile is indistinguishable
    // from an absent one, and 16 KiB more expensive.
    if (any) out.tiles.getOrCreate(coord) = built;
  }
  return out;
}

}  // namespace

// --- PRD E8 ----------------------------------------------------------------

float selectionCoverageFromSignedDistance(float phi) noexcept {
  const float c = phi + 0.5f;
  return c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
}

float selectionSignedDistanceFromCoverage(float coverage) noexcept { return coverage - 0.5f; }

Selection growSelection(const Selection& selection, float radius) {
  Selection out;
  if (selection.tiles.occupiedTileCount() == 0) return out;
  // A non-finite radius returns the input rather than a plane of NaNs. NaN
  // would propagate through `phi + radius` into `writeCoverage`'s clamp, where
  // every comparison is false and the cast is undefined -- an operator that
  // silently produced garbage coverage from one bad UI value.
  if (!std::isfinite(radius)) return selection;

  // **Zero is deliberately NOT special-cased.** Returning a copy would be
  // faster and would make `grow(s, 0) == s` true by construction -- which is
  // exactly why it is not done: that identity is this file's sharpest
  // assertion, and it is only worth anything if it goes through the distance
  // field the way every other radius does. See the header's §2.

  int32_t minTx = 0, minTy = 0, maxTx = 0, maxTy = 0;
  bool first = true;
  for (const auto& [coord, tile] : selection.tiles) {
    (void)tile;
    if (first) {
      minTx = maxTx = coord.x;
      minTy = maxTy = coord.y;
      first = false;
      continue;
    }
    minTx = std::min(minTx, coord.x);
    maxTx = std::max(maxTx, coord.x);
    minTy = std::min(minTy, coord.y);
    maxTy = std::max(maxTy, coord.y);
  }

  // The apron is `ceil(radius) + 1`, and both terms earn their place.
  //
  // `ceil(radius)` is how far a grow can reach: a texel further out than that
  // beyond the input's tile extent has `phi <= -(apron)` even in the worst case
  // where the selection fills its outermost tile, so its grown coverage is
  // `radius - apron < 0`. The `+ 1` is not slack for that -- it is what makes
  // the plane's border provably zero, which the seeding below relies on when it
  // treats an out-of-plane neighbour as unselected. Without it, a selection
  // filling its outermost tile would have its edge sitting on the plane's edge
  // and no hard-transition seed would be found there.
  const int32_t apron = static_cast<int32_t>(std::ceil(std::max(radius, 0.0f))) + 1;
  const PixelCoord origin{minTx * kTileSize - apron, minTy * kTileSize - apron};
  const int32_t w = (maxTx - minTx + 1) * kTileSize + 2 * apron;
  const int32_t h = (maxTy - minTy + 1) * kTileSize + 2 * apron;
  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);

  // The dense plane. Everything outside the input's tiles is exactly 0 -- that
  // is core/SelectionMask's central rule, the inverse of a layer mask -- so
  // blitting the occupied tiles into a zeroed plane is a complete description
  // of the field and not a crop. The same argument `ops/Feather` makes.
  std::vector<float> cov(n, 0.0f);
  for (const auto& [coord, tile] : selection.tiles) {
    const PixelCoord to = tileOrigin(coord);
    const int32_t px0 = to.x - origin.x;
    const int32_t py0 = to.y - origin.y;
    for (int32_t ly = 0; ly < kTileSize; ++ly) {
      float* row = &cov[static_cast<size_t>(py0 + ly) * w + px0];
      for (int32_t lx = 0; lx < kTileSize; ++lx) row[lx] = tile.coverageAt(PixelCoord{lx, ly});
    }
  }

  // Seeding -- the header's §2. A fractional texel knows exactly where the edge
  // is; a saturated one knows only that it is at least half a texel away, so it
  // is allowed to speak only when its neighbour is equally ignorant.
  std::vector<double> seedCost(n, kFar);
  std::vector<float> offset(n, 0.0f);
  bool anySeed = false;
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      const size_t i = static_cast<size_t>(y) * w + x;
      const float c = cov[i];
      if (c > 0.0f && c < 1.0f) {
        seedCost[i] = 0.0;
        offset[i] = selectionSignedDistanceFromCoverage(c);
        anySeed = true;
        continue;
      }
      const bool inside = c >= 0.5f;
      bool hardEdge = false;
      const int32_t nx[4] = {x - 1, x + 1, x, x};
      const int32_t ny[4] = {y, y, y - 1, y + 1};
      for (int k = 0; k < 4; ++k) {
        // Out of plane reads as unselected, which is true rather than
        // defensive: the apron guarantees the border is already zero, so this
        // only ever compares two zeros.
        const bool inPlane = nx[k] >= 0 && nx[k] < w && ny[k] >= 0 && ny[k] < h;
        const float cn = inPlane ? cov[static_cast<size_t>(ny[k]) * w + nx[k]] : 0.0f;
        if (cn != 0.0f && cn != 1.0f) continue;  // fractional: it knows better
        if ((cn >= 0.5f) != inside) hardEdge = true;
      }
      if (hardEdge) {
        seedCost[i] = 0.0;
        // A hard transition puts the contour exactly on the shared texel
        // boundary, half a texel from each centre -- which is also what
        // `selectionSignedDistanceFromCoverage()` returns at the saturated
        // ends, so the two branches agree at the limit rather than by
        // coincidence.
        offset[i] = inside ? 0.5f : -0.5f;
        anySeed = true;
      }
    }
  }
  // Unreachable for a non-empty selection -- some texel is non-zero and the
  // plane's border is zero, so a transition exists -- but a field with no seeds
  // would come back at kFar everywhere and quantise to a full selection, which
  // is the most wrong answer available.
  if (!anySeed) return out;

  const FeatureField field = euclideanFeatureTransform(seedCost, w, h);

  // phi(p) = sign(p) * |p - q*| + offset(q*), then corrected -- see below.
  //
  // A seed is its own feature with distance 0, so its phi is exactly its own
  // offset. That is what makes `grow(s, 0)` return a fractional edge texel
  // bit-for-bit, and the correction below cannot disturb it: `phi` is
  // 1-Lipschitz, so every other seed `q` satisfies `offset(q) + |p-q| >=
  // offset(p)`, and a minimum that already holds the true value cannot be
  // lowered by candidates that all bound it from above.
  std::vector<float> phi(n);
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      const size_t i = static_cast<size_t>(y) * w + x;
      const float d = std::sqrt(static_cast<float>(field.squared[i]));
      const size_t seed = static_cast<size_t>(field.seedY[i]) * w + field.seedX[i];
      const bool inside = cov[i] >= 0.5f;
      const float nearest = (inside ? d : -d) + offset[seed];
      phi[i] = nearest;

      // The band, and it is a cost decision rather than an accuracy one.
      //
      // The correction below moves `phi` by at most 1.0: every candidate is at
      // least `d - 0.5` and the starting value is at most `d + 0.5`. Coverage
      // saturates once `|phi + radius| >= 0.5`, so a texel further than
      // `0.5 + 1.0` from the new contour lands on the same clamped 0 or 1
      // whatever the correction says, and refining it is work with no
      // observable result. That turns an O(texels * window^2) pass into an
      // O(perimeter * window^2) one -- on the disc measured below, 2.5 % of the
      // plane is inside the band.
      constexpr float kRefineBand = 1.5f;
      if (std::fabs(nearest + radius) > kRefineBand) continue;

      // **The correction, and why the exact EDT is not the end of the story.**
      //
      // The field this operator wants is `min over seeds q of (|p-q| +
      // offset(q))` -- an envelope of cones, which is not separable and which
      // FH cannot compute. Taking the seed nearest in plain distance and
      // adding its offset is a different thing, and the difference is not
      // academic: on a curved boundary the nearest seed is routinely one whose
      // offset points the wrong way, and a seed one texel further along the
      // contour with the opposite offset gives the better answer. Measured on
      // a disc of radius 20 grown by 6.5, the uncorrected field is 0.24995 of
      // coverage away from the cone minimum -- 64 uint8 steps, a quarter of a
      // texel of contour position, and visible as a wobble.
      //
      // So the feature transform's answer is used as a *starting point* and
      // the true cone minimum is taken over the seeds within `kRefineWindow`
      // of it. That is exact whenever the cone minimiser lies in the window,
      // and the window size is measured rather than guessed: against a
      // brute-force cone minimum over every seed, growing a disc of radius 20
      // by 2.5 / 6.5 / 12.5 / 20.5 / 30.5,
      //
      //     window 1 -> 0.00196 0.10486 0.14893 0.14488 0.10025
      //     window 2 -> 0.00196 0.00196 0.00775 0.01939 0.01438
      //     window 3 -> 0.00196 0.00196 0.00196 0.00196 0.00196
      //     window 4, 6, 8 -> identical to window 3 at every radius
      //
      // 0.00196 is 1/510, the uint8 store's own half-step: at window 3 the
      // residual is not a distance error at all, it is the store rounding. So
      // 3 is where the sequence stops improving and not a round number
      // someone liked -- and the fact that it does not grow with the radius is
      // worth stating, because the naive bound does (two seeds equidistant to
      // within one offset spread can be up to ~2*sqrt(2*radius) apart along
      // the contour). The bound is loose; the geometry is not.
      float best = nearest;
      const int32_t qx = field.seedX[i], qy = field.seedY[i];
      const int32_t sy0 = std::max(qy - kRefineWindow, 0);
      const int32_t sy1 = std::min(qy + kRefineWindow, h - 1);
      const int32_t sx0 = std::max(qx - kRefineWindow, 0);
      const int32_t sx1 = std::min(qx + kRefineWindow, w - 1);
      for (int32_t sy = sy0; sy <= sy1; ++sy) {
        for (int32_t sx = sx0; sx <= sx1; ++sx) {
          const size_t j = static_cast<size_t>(sy) * w + sx;
          if (seedCost[j] != 0.0) continue;
          const float dx = static_cast<float>(x - sx);
          const float dy = static_cast<float>(y - sy);
          const float dq = std::sqrt(dx * dx + dy * dy);
          // Inside, every seed gives an UPPER bound on phi and the tightest
          // wins; outside, every seed gives a LOWER bound on a negative
          // number and the tightest is the largest. Same 1-Lipschitz argument
          // read from the two sides -- not two rules.
          const float v = inside ? dq + offset[j] : offset[j] - dq;
          best = inside ? std::min(best, v) : std::max(best, v);
        }
      }
      phi[i] = best;
    }
  }

  // Scatter back. Tiles the plane does not reach cannot hold coverage (see the
  // apron argument above), and tile texels outside the plane are zero for the
  // same reason, so the plane's tile span is the whole of the answer.
  const TileCoord firstTile = tileCoordAt(origin);
  const TileCoord lastTile = tileCoordAt(PixelCoord{origin.x + w - 1, origin.y + h - 1});
  for (int32_t ty = firstTile.y; ty <= lastTile.y; ++ty) {
    for (int32_t tx = firstTile.x; tx <= lastTile.x; ++tx) {
      const PixelCoord to = tileOrigin(TileCoord{tx, ty});
      SelectionTile built;
      bool any = false;
      for (int32_t ly = 0; ly < kTileSize; ++ly) {
        const int32_t py = to.y + ly - origin.y;
        if (py < 0 || py >= h) continue;
        for (int32_t lx = 0; lx < kTileSize; ++lx) {
          const int32_t px = to.x + lx - origin.x;
          if (px < 0 || px >= w) continue;
          const float c =
              selectionCoverageFromSignedDistance(phi[static_cast<size_t>(py) * w + px] + radius);
          if (c <= 0.0f) continue;
          built.writeCoverage(PixelCoord{lx, ly}, c);
          any = true;
        }
      }
      // `any` is set from the pre-quantisation coverage, so a tile holding only
      // sub-half-step coverage would survive as 16 KiB of zeros. Re-checked
      // against the stored value instead, which is what the invariant is about.
      if (any && !built.selectsNothing()) out.tiles.getOrCreate(TileCoord{tx, ty}) = built;
    }
  }
  return out;
}

Selection shrinkSelection(const Selection& selection, float radius) {
  return growSelection(selection, -radius);
}

// --- PRD E9 ----------------------------------------------------------------

float selectionLuminanceOf(const std::array<float, 4>& premultiplied) noexcept {
  // core/Premultiply's guard, not a hand-rolled divide: an alpha of zero gives
  // {0,0,0,0} rather than a division by zero, so a never-written texel and a
  // written-transparent one report the same luminance here as they do at the
  // eyedropper and the exporter.
  const std::array<float, 4> straight = unpremultiply(premultiplied);
  // Weights in LINEAR light (their definition, and the domain
  // `ops/PointOps`' other callers use), then ONE encode of the scalar so that
  // the range endpoints live in the domain the user is looking at. The header
  // argues both halves of that order.
  return srgbEncode(computeLuma({straight[0], straight[1], straight[2]}));
}

Selection selectColourRange(const TileStore& source,
                            const std::array<float, 4>& targetStraightLinearRgba, int32_t width,
                            int32_t height, const SelectionRangeParams& params) {
  // Premultiplied once, here, because that is the convention `core::Tile`
  // stores and `floodFillReferenceFrom()` expects; the caller holds a picker's
  // straight colour. Doing it at the boundary rather than at every call site is
  // the same placement `fillThroughSelection()` chose.
  const float a = targetStraightLinearRgba[3];
  const FloodFillReference reference = floodFillReferenceFrom(
      {targetStraightLinearRgba[0] * a, targetStraightLinearRgba[1] * a,
       targetStraightLinearRgba[2] * a, a});

  // `reach` is unused by `floodFillCoverage()` and is set to Global anyway, so
  // that anyone reading this struct sees which of `ops/FloodFill`'s two shapes
  // this operator is -- the non-walking one.
  const FloodFillParams flood{params.tolerance, params.edgeBand, FloodFillReach::Global};
  const auto coverageOf = [&](const std::array<float, 4>& texel) {
    // The shipped metric, called rather than restated. If this ever becomes a
    // hand-written comparison, the equality assertion against a Global flood
    // fill in `--selftest` is what fails.
    return floodFillCoverage(floodFillDistance(reference, texel), flood);
  };

  const bool emptyMatches = coverageOf({0.0f, 0.0f, 0.0f, 0.0f}) > 0.0f;
  return documentPredicatePass(source, width, height, emptyMatches, coverageOf);
}

Selection selectLuminanceRange(const TileStore& source, int32_t width, int32_t height,
                               const SelectionLuminanceRange& range) {
  const auto coverageOf = [&](const std::array<float, 4>& texel) -> float {
    const float alpha = unpremultiply(texel)[3];
    if (alpha <= 0.0f) return 0.0f;  // empty canvas is not a shadow
    const float y = selectionLuminanceOf(texel);
    // How far outside the band, zero inside it. Turning the band into a
    // distance is what lets the ramp be `ops/FloodFill`'s own
    // `floodFillCoverage()` rather than a second falloff shape: with
    // `tolerance == edgeBand` that function is exactly "1.0 at distance 0,
    // linear to 0.0 at the band's width", which is the falloff wanted at each
    // end of the range. An inverted band (low > high) makes both terms
    // positive and selects nothing, which is the intended reading of an empty
    // range.
    const float outside = std::max(std::max(range.low - y, y - range.high), 0.0f);
    const float band = std::max(range.edgeBand, 0.0f);
    // A zero band would make `floodFillCoverage` see tolerance 0 and return 0
    // for every non-exact match, including texels inside the range -- the
    // opposite of the hard-edged answer a zero band means. Handled here rather
    // than by widening that function, whose own zero-tolerance meaning
    // ("exactly this colour") is right for a tolerance and wrong for a band.
    if (band <= 0.0f) return outside <= 0.0f ? alpha : 0.0f;
    return floodFillCoverage(outside, FloodFillParams{band, band, FloodFillReach::Global}) * alpha;
  };

  const bool emptyMatches = false;  // alpha 0 is refused above, always
  return documentPredicatePass(source, width, height, emptyMatches, coverageOf);
}

}  // namespace np
