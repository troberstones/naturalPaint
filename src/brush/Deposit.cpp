#include "brush/Deposit.hpp"

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

// Header §2d: Photoshop's `BlnM` on the `dualBrush` descriptor, applied
// directly to the coverage scalar rather than routed through
// `core/Blend.hpp`'s four-channel pixel/layer blend modes, which have no
// notion of a bare [0,1] coverage float. `base` is the PRIMARY tip's own
// coverage -- the "bottom" of the two, matching Photoshop's own description
// of the second tip as blending ONTO the first.
//
// Both formulas are exactly `0` when `base == 0` (Multiply trivially;
// Overlay's `base < 0.5` branch is `2 * base * second`) -- the identity §2d's
// `dabPixelBounds()` argument rests on, so it is stated here rather than only
// in the header, next to the two lines that make it true.
float combineDualCoverage(DualBrushBlend mode, float base, float second) noexcept {
  switch (mode) {
    case DualBrushBlend::Multiply:
      return base * second;
    case DualBrushBlend::Overlay:
      return base < 0.5f ? 2.0f * base * second
                         : 1.0f - 2.0f * (1.0f - base) * (1.0f - second);
  }
  return base;  // unreachable for a valid enumerator; see DualBrushBlend's own comment.
}

}  // namespace

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

          const float cov = dabCoverage(tip, dx, dy);
          if (!(cov > 0.0f)) continue;
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
