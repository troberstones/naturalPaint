#include "brush/Deposit.hpp"

#include "brush/CoverageBlend.hpp"

#include <algorithm>
#include <cmath>

#include "core/Blend.hpp"

namespace np {
namespace {

float degToRad(float deg) noexcept { return deg * 0.017453292519943295f; }

// Document pixels per native bitmap texel, for a tip currently at `radius`
// (header §2c point 2): whichever of the bitmap's own width/height is larger
// maps onto `radius` exactly, so this is the one number that scales both axes
// consistently. Zero when the bitmap is degenerate (should not occur --
// io/AbrBrushes.cpp never builds a zero-dimension `BrushTipBitmap` -- but a
// tip is untrusted-file-derived data by the time it reaches here, so this is
// checked rather than assumed).
float bitmapTipScale(const BrushTipBitmap& bmp, float radius) noexcept {
  const float nativeHalfMax =
      0.5f * static_cast<float>(std::max(bmp.width, bmp.height));
  return nativeHalfMax > 0.0f ? radius / nativeHalfMax : 0.0f;
}

// Bilinear sample of a bitmap tip's coverage, `bx`/`by` already checked by
// the caller to lie in `[0,width] x [0,height]`. Texel-CENTRE convention,
// `(x+0.5, y+0.5)`, matching `dabPixelBounds()` -- so a query landing exactly
// on a stored texel's centre returns that texel's value with zero blend,
// which is what lets `--selftest` assert exact fixture values rather than
// only a tolerance. The four texels around a query are clamped to the
// bitmap's own edge (not treated as transparent beyond it), which is what
// keeps the mapped rectangle's own border crisp rather than feathering it by
// half a texel for free.
float sampleBitmapCoverage(const BrushTipBitmap& bmp, float bx, float by) noexcept {
  const float fx = bx - 0.5f;
  const float fy = by - 0.5f;
  const int32_t w = bmp.width;
  const int32_t h = bmp.height;
  const int32_t x0 = static_cast<int32_t>(std::floor(fx));
  const int32_t y0 = static_cast<int32_t>(std::floor(fy));
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);
  const auto at = [&](int32_t x, int32_t y) -> float {
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    return static_cast<float>(
               bmp.alpha[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)]) /
           255.0f;
  };
  const float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
  const float bot = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
  return top + (bot - top) * ty;
}

// Header §2c: rotate and squash exactly as §2b does, then map the isotropic
// `[-radius, radius]` square onto the bitmap's own rectangle, independently
// per axis. Zero outside `[0,width] x [0,height]` -- a bitmap tip's own
// rectangle IS its "outside the disc" test, so there is no separate radial
// gate the way the procedural branch has one.
float bitmapDabCoverage(const BrushTipBitmap& bmp, const BrushTip& tip, float dx,
                        float dy) noexcept {
  const float scale = bitmapTipScale(bmp, tip.radius);
  if (!(scale > 0.0f)) return 0.0f;

  float u = dx;
  float v = dy;
  if (tip.angle != 0.0f) {
    const float t = degToRad(tip.angle);
    const float c = std::cos(t);
    const float s = std::sin(t);
    u = dx * c + dy * s;
    v = -dx * s + dy * c;
  }
  const float rn = std::clamp(tip.roundness, kMinRoundness, 1.0f);
  v /= rn;

  const float bx = static_cast<float>(bmp.width) * 0.5f + u / scale;
  const float by = static_cast<float>(bmp.height) * 0.5f + v / scale;
  if (bx < 0.0f || by < 0.0f || bx > static_cast<float>(bmp.width) ||
      by > static_cast<float>(bmp.height))
    return 0.0f;
  return sampleBitmapCoverage(bmp, bx, by);
}

// A single tip's own coverage profile -- §2/§2b/§2c, and everything
// `dabCoverage()` did before §2d existed, verbatim. **Deliberately does not
// read `tip.dualTip` or `tip.dualBlend`, and that omission is the whole of
// §2d's no-recursion guarantee**: `dabCoverage()` below calls this once on its
// own `tip` and, when a dual tip is present, once more on `*tip.dualTip` --
// and because this function never looks past the tip it is handed, a second
// (or third) level of nesting on that inner tip is never visited, regardless
// of what is stored there.
float singleTipCoverage(const BrushTip& tip, float dx, float dy) noexcept {
  const float r = tip.radius;
  if (!(r > 0.0f)) return 0.0f;

  // A sampled tip replaces the whole procedural profile below -- header
  // §2c's opening argument. The size/alpha check guards a `BrushTipBitmap`
  // that failed to decode cleanly (io/AbrBrushes.cpp never hands one back in
  // that state, but a tip is untrusted-file-derived data by the time it
  // reaches here, and falling through to the round tip is a safer failure
  // than indexing an empty `alpha`).
  if (tip.bitmap != nullptr && tip.bitmap->width > 0 && tip.bitmap->height > 0 &&
      tip.bitmap->alpha.size() ==
          static_cast<size_t>(tip.bitmap->width) * static_cast<size_t>(tip.bitmap->height)) {
    return bitmapDabCoverage(*tip.bitmap, tip, dx, dy);
  }

  // --- The offset, in the tip's own frame (header §2b) --------------------
  //
  // **A round tip takes the else branch and nothing else**, so `d2` is the
  // bit-identical float it was before the ellipse existed. Both guards matter
  // and neither is an optimisation: the rotation is skipped for a circle
  // because rotating an isotropic distance is a no-op in exact arithmetic and
  // is *not* one in floating point, and skipping it is what keeps every
  // already-deposited dab, `--pigment-stroke-demo` and the `canvas` golden
  // exactly where they were.
  const float rn = std::clamp(tip.roundness, kMinRoundness, 1.0f);
  float d2;
  if (rn < 1.0f) {
    float u = dx;
    float v = dy;
    if (tip.angle != 0.0f) {
      // Degrees on the wire because that is what the ANGLE slider shows, what
      // `DynamicTarget::Angle` adds in, and what Photoshop's `Angl` imports
      // as. The conversion happens here, once, rather than being a second
      // unit for BrushState to disagree with app/AppState about.
      const float t = tip.angle * 0.017453292519943295f;  // pi / 180
      const float c = std::cos(t);
      const float s = std::sin(t);
      u = dx * c + dy * s;
      v = -dx * s + dy * c;
    }
    // The minor semi-axis is `rn * r`, so a point at `|v| == rn*r` must land
    // on the rim: dividing by `rn` is what puts it at `r` in the isotropic
    // measure the profile below is written in.
    v /= rn;
    d2 = u * u + v * v;
  } else {
    // Squared, before any square root: this is the comparison the footprint
    // argument (header §3, fact 1) rests on, and it must be the *only* thing
    // that decides whether a texel is outside the dab. A `sqrt` first would
    // put a rounding between the disc and the test.
    d2 = dx * dx + dy * dy;
  }
  const float r2 = r * r;
  if (!(d2 < r2)) return 0.0f;

  const float h = std::clamp(tip.hardness, 0.0f, 1.0f);
  const float d = std::sqrt(d2) / r;  // in [0,1)
  if (d <= h) return 1.0f;
  // h < d < 1 here, so h < 1 and the divisor is strictly positive -- the one
  // division in this function, unreachable for the hard-disc tip.
  const float u = (d - h) / (1.0f - h);
  return 1.0f - u * u * (3.0f - 2.0f * u);
}

}  // namespace

// Not in the anonymous namespace above, and declared in brush/Deposit.hpp --
// **exposed so `--selftest` can drive it directly**, the same discipline
// `core/LayerGeometry.hpp`'s `translatedTileStore()` already uses for a
// tile-level operation a higher-level entry point would otherwise hide. The
// reason this one specifically needs it: `dabCoverage()` below never calls
// this function with `base == 0` at all (its own short-circuit, a few lines
// down, returns `0.0f` before evaluating any blend mode) -- so the `base ==
// 0` identity this function's own comment documents for EVERY member,
// including Hard Mix's explicit guard, would otherwise be provable only by
// inspection, and a regression in the guard would go unnoticed by any test
// that can only observe `dabCoverage()`'s already-masked output.
//
// Header §2d: Photoshop's `BlnM` on the `dualBrush` descriptor, applied
// directly to the coverage scalar rather than routed through
// `core/Blend.hpp`'s four-channel pixel/layer blend modes, which have no
// notion of a bare [0,1] coverage float. `base` is the PRIMARY tip's own
// coverage -- the "bottom" of the two, matching Photoshop's own description
// of the second tip as blending ONTO the first.
//
// **Reusing Photoshop's per-CHANNEL formulas on a bare coverage scalar is a
// MODELLING decision, not algebra**, and it is made the same way for all
// four members: a coverage value is "how much of the dab lands here," not a
// colour channel, and nothing forces the two to share a formula. It is done
// anyway because the alternative is a second, invented blend vocabulary that
// nobody asked for, and because -- for three of the four members below -- the
// standard formula already happens to preserve the one property this module
// actually needs.
//
// **All four are exactly `0` when `base == 0`**, and that is what §2d's
// `dabPixelBounds()` argument rests on -- restated here, next to the lines
// that make each one true, rather than only in the header:
//   * Multiply: trivially, `0 * second == 0`.
//   * Overlay: the `base < 0.5` branch is `2 * base * second`, which is `0`
//     for any `second` when `base == 0`.
//   * Color Burn: at `base == 0` the formula is `1 - min(1, 1/second)`. For
//     every `second` in `(0, 1]`, `1/second >= 1`, so the `min` clamps to `1`
//     and the whole expression is `0` -- the STANDARD formula already has
//     this identity, nothing added for it.
//   * Hard Mix: the ONE mode where the standard formula does NOT have the
//     identity for free -- `base + second >= 1` is true at `base == 0,
//     second == 1`, which the bare formula would answer `1`, painting where
//     the primary tip has no coverage at all. The `base > 0.0f` guard in that
//     case is not part of Photoshop's own formula; it is this function's own
//     fix, made explicitly rather than left to the `dabCoverage()` caller's
//     own short-circuit (which also holds, defense in depth, but this
//     function documents and asserts the identity as its own).
//
// **Hard Mix's threshold is binary, and that is a real cost, not a bug left
// in.** Every other coverage this module produces is continuous --
// `singleTipCoverage()`'s smoothstep (§2) exists specifically so a dab's rim
// antialiases -- and Hard Mix's `>= 1.0f ? 1.0f : 0.0f` throws that away at
// the COMBINED mark's own edge: wherever `base + second` crosses `1`, the
// output jumps from 0 to 1 with no smoothstep between, however soft `base`
// and `second` individually are. That is not a modelling mistake for a Dual
// Brush -- it is what Photoshop's own Hard Mix does in a layer stack as much
// as here, it is why Hard Mix reads as a harsh, posterised mode there too,
// and for a Dual Brush specifically it is why a preset using it looks
// grittier than the identical two tips combined by Multiply or Overlay: the
// binary threshold is what breaks the primary tip's smooth rim into a
// jagged one, which this file's header calls "most of why an ink brush
// reads granular rather than smooth" (§2d, opening paragraph).
float combineDualCoverage(DualBrushBlend mode, float base, float second) noexcept {
  // **The formulas moved to brush/CoverageBlend.cpp, unchanged.** The Dual
  // Brush and the Texture panel ask the same question -- combine two coverage
  // values into one -- and this function's four modes were four of the ten
  // the two panels between them name across the packs measured. Both of the
  // guards this function carried and the shared one did not (Color Burn's
  // saturated-base case, and Hard Mix's `base > 0`) went with them, because
  // both are correct for grain too.
  //
  // Kept as a named function rather than replaced at every call site so that
  // nothing about the dual-brush path churns for a change that is about where
  // the arithmetic lives.
  return applyCoverageBlend(mode, base, second);
}

bool brushTipEqual(const BrushTip& a, const BrushTip& b) noexcept {
  // **The completeness guard is the structured binding, not a `sizeof`.** A
  // structured binding must name EVERY member of an aggregate -- too few or too
  // many and it does not compile -- so adding a field to `BrushTip` is a build
  // error pointing at this line, and the fix is to name it here and compare it
  // below. Every binding is then used in the comparison, so the two cannot
  // drift apart: a named-but-uncompared field would be an unused binding, which
  // `-Werror=unused-variable` (src/CMakeLists.txt) rejects.
  //
  // A `static_assert(sizeof(BrushTip) == N)` was written here first and is NOT
  // good enough: adding a `float` after `sizeFloorPx` (since removed --
  // brush/Variance.hpp now owns that floor, applied inside its own formula)
  // once landed it in the struct's existing tail padding, left `sizeof` at
  // 136, and the guard passed while the new field went uncompared. Tested,
  // not assumed -- which is the only reason the weaker version is not still
  // here.
  const auto& [radius, hardness, roundness, angle, bitmap, dualTip, dualBlend, flow, spacing,
               scatter, scatterBothAxes, grain, pigment, linearRgb, opacity] = a;
  // `bitmap` and `dualTip` compare by POINTER, which is `dabPreviewTipsEqual()`'s
  // established convention; its comment carries the argument.
  return radius == b.radius && hardness == b.hardness && roundness == b.roundness &&
         angle == b.angle && bitmap == b.bitmap && dualTip == b.dualTip &&
         dualBlend == b.dualBlend && flow == b.flow && spacing == b.spacing &&
         scatter == b.scatter && scatterBothAxes == b.scatterBothAxes &&
         grainParamsEqual(grain, b.grain) && pigment == b.pigment &&
         linearRgb == b.linearRgb && opacity == b.opacity;
}

float dabCoverage(const BrushTip& tip, float dx, float dy) noexcept {
  const float base = singleTipCoverage(tip, dx, dy);
  if (tip.dualTip == nullptr) return base;

  // §2d: exactly 0, not merely computed-and-equal-to-0 -- this is what keeps
  // `dabPixelBounds()` correct without a dual-brush case of its own, AND what
  // skips sampling the second tip (a bitmap sample is real work) for every
  // texel the primary tip's own disc already excludes.
  if (!(base > 0.0f)) return 0.0f;

  // §2d's no-recursion guarantee: `singleTipCoverage()` never reads
  // `dualTip->dualTip`, so this call cannot recurse regardless of what a
  // malformed or hand-built `BrushTip` tree stores past this level.
  const float second = singleTipCoverage(*tip.dualTip, dx, dy);
  return std::clamp(combineDualCoverage(tip.dualBlend, base, second), 0.0f, 1.0f);
}

PigmentTexel depositTexel(const PigmentTexel& dst, const Latent& pigment, float deltaMass,
                          float selection) noexcept {
  const float denom = dst.mass + deltaMass;
  // Header §1(ii): the limit as dm -> 0+ on empty paper, not a convention.
  const float w = (denom > 0.0f) ? (deltaMass / denom) : 1.0f;

  PigmentTexel out;
  // core/Blend's own lerp, not a second copy of it: `Mix` and the brush must
  // agree about what mixing two latents means, and this is the same function
  // core/Composite's Pigment-pair branch calls.
  //
  // **Deliberately weighted by the UNCAPPED `dm`**, header §1(iii) and §4: a
  // texel that has reached its cap -- the paper's or the selection's -- keeps
  // taking on the brush's hue. The selection bounds how much paint is present,
  // not which paint it is.
  out.latent = mixLatents(dst.latent, pigment, w);

  // Header §4. The cap is the paper's capacity scaled by the selection's
  // coverage, with two clauses the header argues at length:
  //
  //   * never below `dst.mass` -- a DEPOSIT must never remove paint, so a texel
  //     already thicker than the selection allows keeps what it has and simply
  //     gains nothing. Without this the brush erases wherever a selection is
  //     thinner than the paint under it.
  //   * never above `kMaxMass` -- `sel <= 1` so this bites only for a
  //     destination handed in already over the cap, and clamping at the point
  //     of storage is what makes the invariant a property of the document.
  //
  // At `sel == 1` this is `min(denom, kMaxMass)` bit for bit, which is what the
  // rule was before the parameter existed.
  const float sel = std::clamp(selection, 0.0f, 1.0f);
  float cap = kMaxMass * sel;
  if (cap < dst.mass) cap = dst.mass;
  if (cap > kMaxMass) cap = kMaxMass;
  out.mass = denom < cap ? denom : cap;
  return out;
}

PixelBounds dabPixelBounds(const BrushTip& tip, Vec2 centre, int32_t canvasW,
                           int32_t canvasH) noexcept {
  PixelBounds b;
  if (!(tip.radius > 0.0f) || canvasW <= 0 || canvasH <= 0) return b;

  // Texel (x,y) is sampled at (x+0.5, y+0.5), so coverage can be non-zero only
  // for |x + 0.5 - cx| < r. Floor/ceil of the open interval, then clipped.
  const float r = tip.radius;

  // Symmetric for every existing tip -- round, elliptical, angled -- exactly
  // as before this pair existed (header §2b's "not tightened" argument).
  // **Only a bitmap tip (§2c) can make these two different from each other
  // and from `r`**: a rotated non-square sample's axis-aligned box is wider
  // than its own un-rotated footprint, computed here from the standard
  // rotated-rectangle formula rather than reused symmetrically. The `.bitmap`
  // branch below is therefore the only place `dabPixelBounds()` can disagree
  // with the identical computation it did before this feature existed.
  float halfX = r;
  float halfY = r;
  if (tip.bitmap != nullptr && tip.bitmap->width > 0 && tip.bitmap->height > 0) {
    const float scale = bitmapTipScale(*tip.bitmap, r);
    if (scale > 0.0f) {
      const float bw = static_cast<float>(tip.bitmap->width) * 0.5f * scale;
      const float bh = static_cast<float>(tip.bitmap->height) * 0.5f * scale;
      if (tip.angle != 0.0f) {
        const float t = degToRad(tip.angle);
        const float c = std::abs(std::cos(t));
        const float s = std::abs(std::sin(t));
        halfX = bw * c + bh * s;
        halfY = bw * s + bh * c;
      } else {
        halfX = bw;
        halfY = bh;
      }
    }
  }

  const auto lo = [](float v) { return static_cast<int32_t>(std::floor(v)); };
  const auto hi = [](float v) { return static_cast<int32_t>(std::ceil(v)); };
  b.x0 = std::max<int32_t>(0, lo(centre.x - halfX - 0.5f));
  b.y0 = std::max<int32_t>(0, lo(centre.y - halfY - 0.5f));
  b.x1 = std::min<int32_t>(canvasW - 1, hi(centre.x + halfX - 0.5f));
  b.y1 = std::min<int32_t>(canvasH - 1, hi(centre.y + halfY - 0.5f));
  return b;
}

DepositCount depositDab(PigmentTileStore& store, const BrushTip& tip, Vec2 centre,
                        int32_t canvasW, int32_t canvasH, const Selection* selection,
                        std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  if (!(tip.flow > 0.0f)) return count;

  const PixelBounds b = dabPixelBounds(tip, centre, canvasW, canvasH);
  if (b.empty()) return count;

  const TileCoord first = tileCoordAt(PixelCoord{b.x0, b.y0});
  const TileCoord last = tileCoordAt(PixelCoord{b.x1, b.y1});

  // Tile-major, then texel within tile: one hash lookup per tile per dab
  // rather than one per texel, and the tile pointer stays hot for its whole
  // sub-rectangle -- and with a selection there are now *two* stores keyed by
  // the same coordinate, so hoisting saves two lookups per texel rather than
  // one. Ascending (y, x) so `touchedOut` comes out in `sortUniqueTiles()`'s
  // order for the common single-dab case.
  for (int32_t ty = first.y; ty <= last.y; ++ty) {
    for (int32_t tx = first.x; tx <= last.x; ++tx) {
      const TileCoord coord{tx, ty};

      // §4. The null branch is owned here rather than borrowed from
      // `selectionCoverageAt()`, which core/SelectionMask.hpp requires of every
      // hoisted loop -- a null *Selection* is "no restriction" and a null
      // *tile* inside an engaged selection is "selects nothing". Getting those
      // two nulls the same way round is how a brush starts painting outside the
      // ants, or stops painting at all.
      const SelectionTile* cover = nullptr;
      if (selection != nullptr) {
        cover = selection->tiles.find(coord);
        // An engaged selection that names no tile here selects nothing here, so
        // the whole tile is skipped before anything is looked up or allocated.
        if (cover == nullptr) continue;
      }

      const PixelCoord org = tileOrigin(coord);
      const int32_t x0 = std::max(b.x0, org.x);
      const int32_t x1 = std::min(b.x1, org.x + kTileSize - 1);
      const int32_t y0 = std::max(b.y0, org.y);
      const int32_t y1 = std::min(b.y1, org.y + kTileSize - 1);

      // Fetched lazily, at the first texel this dab actually changes inside
      // this tile -- header §3, fact 2. A tile the bounding box clipped but
      // the disc missed is never created and never reported.
      PigmentTile* tile = nullptr;

      for (int32_t y = y0; y <= y1; ++y) {
        const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
        for (int32_t x = x0; x <= x1; ++x) {
          const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
          const PixelCoord local = tileLocalOffset(PixelCoord{x, y});

          const float rawCov = dabCoverage(tip, dx, dy);
          if (!(rawCov > 0.0f)) continue;

          // §2e: grain modulates the tip's own coverage at this texel's
          // ABSOLUTE canvas position -- `x`/`y` here, not `dx`/`dy` above,
          // which is why this cannot live inside `dabCoverage()` itself. A
          // texel `dabCoverage()` already excluded never reaches this line
          // (the `continue` above), so grain can only thin or empty a texel
          // already inside the footprint §3 bounds, never add one outside
          // it. `grainCoverageAt()` returns `rawCov` bit-identical when
          // `tip.grain` is off (its own default), which is what keeps this
          // line a no-op for every brush that has not turned grain on.
          const float cov = grainCoverageAt(tip.grain, rawCov, x, y);
          if (!(cov > 0.0f)) continue;  // a grain peak too tall for this pressure

          const float sel = selection != nullptr ? selectionTileCoverage(cover, local) : 1.0f;
          // A texel the selection excludes is not written AT ALL -- not written
          // with zero mass, not counted, and its tile not created on its
          // account. That is what keeps §3's two facts true with a selection in
          // play: the reported tile set stays exactly the set of tiles whose
          // bytes changed.
          if (!(sel > 0.0f)) continue;

          // **The selection enters TWICE, and both are load-bearing** -- §4.
          // Into the rate, so one pass through a half-selected texel lays half
          // a dab; and into the cap, so *no number of passes* takes that texel
          // past half. The first alone is a speed limit rather than a bound,
          // and at the shipped defaults a half-selected texel walks straight
          // through it in six dabs -- one and a half radii of travel, which is
          // less than one ordinary brush-width of a stroke.
          const float deltaMass = tip.flow * cov * sel;
          if (!(deltaMass > 0.0f)) continue;

          if (tile == nullptr) {
            tile = &store.getOrCreate(coord);
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
          }
          tile->writeTexel(local,
                           depositTexel(tile->readTexel(local), tip.pigment, deltaMass, sel));
          ++count.texels;
        }
      }
    }
  }
  return count;
}

void sortUniqueTiles(std::vector<TileCoord>& tiles) {
  std::sort(tiles.begin(), tiles.end(), [](const TileCoord& a, const TileCoord& b) {
    return a.y != b.y ? a.y < b.y : a.x < b.x;
  });
  tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
}

StrokeDeposit depositDabs(PigmentTileStore& store, const BrushTip& tip,
                          const std::vector<Vec2>& dabs, int32_t canvasW, int32_t canvasH,
                          const Selection* selection) {
  StrokeDeposit out;
  for (const Vec2& dab : dabs) {
    const DepositCount c = depositDab(store, tip, dab, canvasW, canvasH, selection, &out.tiles);
    out.texels += c.texels;
    ++out.dabs;
  }
  sortUniqueTiles(out.tiles);
  return out;
}

}  // namespace np
