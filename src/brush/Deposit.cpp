#include "brush/Deposit.hpp"

#include <algorithm>
#include <cmath>

#include "core/Blend.hpp"

namespace np {

float dabCoverage(const BrushTip& tip, float dx, float dy) noexcept {
  const float r = tip.radius;
  if (!(r > 0.0f)) return 0.0f;

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

PigmentTexel depositTexel(const PigmentTexel& dst, const Latent& pigment,
                          float deltaMass) noexcept {
  const float denom = dst.mass + deltaMass;
  // Header §1(ii): the limit as dm -> 0+ on empty paper, not a convention.
  const float w = (denom > 0.0f) ? (deltaMass / denom) : 1.0f;

  PigmentTexel out;
  // core/Blend's own lerp, not a second copy of it: `Mix` and the brush must
  // agree about what mixing two latents means, and this is the same function
  // core/Composite's Pigment-pair branch calls.
  out.latent = mixLatents(dst.latent, pigment, w);
  out.mass = denom < kMaxMass ? denom : kMaxMass;
  return out;
}

PixelBounds dabPixelBounds(const BrushTip& tip, Vec2 centre, int32_t canvasW,
                           int32_t canvasH) noexcept {
  PixelBounds b;
  if (!(tip.radius > 0.0f) || canvasW <= 0 || canvasH <= 0) return b;

  // Texel (x,y) is sampled at (x+0.5, y+0.5), so coverage can be non-zero only
  // for |x + 0.5 - cx| < r. Floor/ceil of the open interval, then clipped.
  const float r = tip.radius;
  const auto lo = [](float v) { return static_cast<int32_t>(std::floor(v)); };
  const auto hi = [](float v) { return static_cast<int32_t>(std::ceil(v)); };
  b.x0 = std::max<int32_t>(0, lo(centre.x - r - 0.5f));
  b.y0 = std::max<int32_t>(0, lo(centre.y - r - 0.5f));
  b.x1 = std::min<int32_t>(canvasW - 1, hi(centre.x + r - 0.5f));
  b.y1 = std::min<int32_t>(canvasH - 1, hi(centre.y + r - 0.5f));
  return b;
}

DepositCount depositDab(PigmentTileStore& store, const BrushTip& tip, Vec2 centre,
                        int32_t canvasW, int32_t canvasH,
                        std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  if (!(tip.flow > 0.0f)) return count;

  const PixelBounds b = dabPixelBounds(tip, centre, canvasW, canvasH);
  if (b.empty()) return count;

  const TileCoord first = tileCoordAt(PixelCoord{b.x0, b.y0});
  const TileCoord last = tileCoordAt(PixelCoord{b.x1, b.y1});

  // Tile-major, then texel within tile: one hash lookup per tile per dab
  // rather than one per texel, and the tile pointer stays hot for its whole
  // sub-rectangle. Ascending (y, x) so `touchedOut` comes out in
  // `sortUniqueTiles()`'s order for the common single-dab case.
  for (int32_t ty = first.y; ty <= last.y; ++ty) {
    for (int32_t tx = first.x; tx <= last.x; ++tx) {
      const TileCoord coord{tx, ty};
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
          const float deltaMass = tip.flow * dabCoverage(tip, dx, dy);
          if (!(deltaMass > 0.0f)) continue;

          if (tile == nullptr) {
            tile = &store.getOrCreate(coord);
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
          }
          const PixelCoord local = tileLocalOffset(PixelCoord{x, y});
          tile->writeTexel(local, depositTexel(tile->readTexel(local), tip.pigment, deltaMass));
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
                          const std::vector<Vec2>& dabs, int32_t canvasW,
                          int32_t canvasH) {
  StrokeDeposit out;
  for (const Vec2& dab : dabs) {
    const DepositCount c = depositDab(store, tip, dab, canvasW, canvasH, &out.tiles);
    out.texels += c.texels;
    ++out.dabs;
  }
  sortUniqueTiles(out.tiles);
  return out;
}

}  // namespace np
