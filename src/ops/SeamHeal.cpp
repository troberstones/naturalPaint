#include "ops/SeamHeal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

#include "core/SelectionMask.hpp"
#include "ops/Filters.hpp"
#include "ops/PatchMatch.hpp"

// The argument is in ops/SeamHeal.hpp. What is here is the four steps it
// names: offset, build the band mask, PatchMatch under wrap addressing,
// offset back.
namespace np {
namespace {

// Euclidean modulus -- ops/Filters.hpp's own rule for wrap addressing:
// `-1 % period` must land on `period - 1`, not `-1`.
int32_t wrapMod(int32_t v, int32_t period) noexcept {
  const int32_t m = v % period;
  return m < 0 ? m + period : m;
}

// Marks one full-height (or full-width) band of the torus as hole, centred
// on `centre` along `axis` (0 = vertical band varying in x, 1 = horizontal
// band varying in y), `width` texels wide, wrapping at the canvas edge --
// the seam is a line on a torus, and a band across it wraps exactly the way
// the seam itself does.
void markBand(Selection& sel, const PixelRect& wrapRect, int32_t centre, int32_t width,
             int axis) {
  const int32_t w = wrapRect.width();
  const int32_t h = wrapRect.height();
  const int32_t lo = -(width / 2);
  const int32_t hi = width - (width / 2);  // width texels total: [lo, hi)
  if (axis == 0) {
    for (int32_t dx = lo; dx < hi; ++dx) {
      const int32_t x = wrapMod(centre + dx, w);
      for (int32_t y = 0; y < h; ++y) {
        const PixelCoord doc{x, y};
        sel.tiles.getOrCreate(tileCoordAt(doc)).writeCoverage(tileLocalOffset(doc), 1.0f);
      }
    }
  } else {
    for (int32_t dy = lo; dy < hi; ++dy) {
      const int32_t y = wrapMod(centre + dy, h);
      for (int32_t x = 0; x < w; ++x) {
        const PixelCoord doc{x, y};
        sel.tiles.getOrCreate(tileCoordAt(doc)).writeCoverage(tileLocalOffset(doc), 1.0f);
      }
    }
  }
}

// A copy of `shifted` plus a halo of `pad` texels beyond each of the four
// canvas edges, filled by wrapping (mod `wrapRect`) back into `shifted`
// itself -- so a PatchMatch search rectangle that reaches past the canvas
// edge finds the texture that would be there on a tiled torus, rather than
// the transparent black an absent tile would otherwise read as. Only the
// halo is wrap-sourced; the interior is `shifted`'s own tiles (shared, not
// copied).
TileStore buildWrappedSource(const TileStore& shifted, const PixelRect& wrapRect, int32_t pad) {
  TileStore out = shifted;
  const int32_t w = wrapRect.width();
  const int32_t h = wrapRect.height();
  if (pad <= 0 || w <= 0 || h <= 0) return out;

  auto copyWrapped = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    for (int32_t y = y0; y < y1; ++y) {
      const int32_t sy = wrapMod(y, h);
      for (int32_t x = x0; x < x1; ++x) {
        const int32_t sx = wrapMod(x, w);
        const PixelCoord src{sx, sy};
        const Tile* t = shifted.find(tileCoordAt(src));
        const std::array<float, 4> rgba =
            t == nullptr ? std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}
                        : t->readPixel(tileLocalOffset(src));
        const PixelCoord dstCoord{x, y};
        out.getOrCreate(tileCoordAt(dstCoord)).writePixel(tileLocalOffset(dstCoord), rgba);
      }
    }
  };
  // Top and bottom strips span the full padded width, so they carry the four
  // corners; left and right need only the interior height.
  copyWrapped(-pad, -pad, w + pad, 0);
  copyWrapped(-pad, h, w + pad, h + pad);
  copyWrapped(-pad, 0, 0, h);
  copyWrapped(w, 0, w + pad, h);
  return out;
}

}  // namespace

bool seamHealParamsValid(const SeamHealParams& p) noexcept {
  if (roiIsEmpty(p.wrapRect)) return false;
  if (p.bandWidth < 1) return false;
  if (p.patchRadius < 1 || p.patchRadius > kPatchMatchMaxPatchRadius) return false;
  if (p.iterations < 1 || p.iterations > kPatchMatchMaxIterations) return false;
  return p.pyramidLevels >= 1 && p.pyramidLevels <= kPatchMatchMaxPyramidLevels;
}

PixelCoord seamHealCentre(const PixelRect& wrapRect) noexcept {
  // Exactly `applyOffset()`'s `offsetByHalf()`: floor of a non-negative
  // extent, so there is no round-toward-zero case to differ on.
  return PixelCoord{wrapRect.width() / 2, wrapRect.height() / 2};
}

bool seamHealTiles(const TileStore& src, const PixelRect& outRect, const SeamHealParams& p,
                   TileStore* dst) {
  if (dst == nullptr || dst == &src) return false;
  if (!seamHealParamsValid(p)) return false;
  if (!(outRect == p.wrapRect)) return false;

  const PixelCoord centre = seamHealCentre(p.wrapRect);

  TileStore shifted;
  const OffsetParams forward{centre.x, centre.y, OffsetEdge::Wrap, p.wrapRect};
  if (!offsetTiles(src, p.wrapRect, forward, &shifted)) return false;

  // The search margin: generous enough to find real texture away from the
  // band, capped the same way ops/PatchMatch.hpp caps every caller's.
  const int32_t margin =
      std::min(kPatchMatchMaxSourceMargin, std::max(p.bandWidth * 4, 64));
  const int32_t halo = margin + p.patchRadius + 1;

  PatchMatchParams pm;
  pm.patchRadius = p.patchRadius;
  pm.iterations = p.iterations;
  pm.pyramidLevels = p.pyramidLevels;
  pm.sourceMargin = margin;
  pm.seed = p.seed;

  // **Two sequential PatchMatch calls, not one mask over both bands.** A
  // vertical band union a horizontal band spans the WHOLE canvas by bounding
  // box -- the vertical band alone reaches every row, the horizontal alone
  // reaches every column -- so a single hole built from both would blow
  // ops/PatchMatch.hpp's bbox-sized cap on any real document. Each band
  // ALONE has a tight bbox (`bandWidth` by the canvas's other dimension), so
  // healing them one at a time keeps every call inside the cap. The vertical
  // pass runs first and the horizontal pass reads its result as source,
  // including at the crossing, which the horizontal pass then re-fills --
  // the crossing ends up healed by whichever pass ran last, not left with a
  // visible seam between the two.
  Selection vertical;
  markBand(vertical, p.wrapRect, centre.x, p.bandWidth, /*axis=*/0);
  pm.hole = &vertical;
  const TileStore sourceForVertical = buildWrappedSource(shifted, p.wrapRect, halo);
  TileStore healedVertical = shifted;
  if (!patchMatchTiles(sourceForVertical, p.wrapRect, pm, &healedVertical)) return false;

  Selection horizontal;
  markBand(horizontal, p.wrapRect, centre.y, p.bandWidth, /*axis=*/1);
  pm.hole = &horizontal;
  const TileStore sourceForHorizontal = buildWrappedSource(healedVertical, p.wrapRect, halo);
  TileStore healedBoth = healedVertical;
  if (!patchMatchTiles(sourceForHorizontal, p.wrapRect, pm, &healedBoth)) return false;

  // Offset back -- the inverse of `forward`, so every texel outside the two
  // bands round-trips to bit-identical (ops/Filters.hpp section 4: both legs
  // are pure addressing changes).
  const OffsetParams backward{-centre.x, -centre.y, OffsetEdge::Wrap, p.wrapRect};
  return offsetTiles(healedBoth, outRect, backward, dst);
}

}  // namespace np
