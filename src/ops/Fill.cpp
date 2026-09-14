#include "ops/Fill.hpp"

#include <cmath>

#include "ops/Gradient.hpp"

namespace np {

bool fillParamsValid(const FillParams& p) noexcept {
  if (!std::isfinite(p.opacity) || p.opacity < 0.0f) return false;
  // `Mix` has no arithmetic here to fall back to (`core/Blend.hpp`'s own
  // `blendPixel()` falls back to `over` for it, silently) -- refused by name
  // rather than silently composited as Normal, so a fill recorded with
  // "blend": "mix" reads as the refusal it is rather than as a Normal fill
  // that happened to say something else.
  if (p.blend == BlendMode::Mix) return false;
  switch (p.source) {
    case FillSource::Color:
      for (float c : p.color)
        if (!std::isfinite(c)) return false;
      return true;
    case FillSource::Pattern:
      return p.pattern != nullptr && p.pattern->valid();
    case FillSource::Gradient:
      // `ops/Gradient.hpp`'s own rule: zero colour stops renders nothing,
      // zero opacity stops is fully opaque. Both are legitimate ramps, so
      // there is nothing further to validate here.
      return true;
  }
  return false;
}

bool fillTiles(const TileStore& src, const PixelRect& outRect, const FillParams& p,
               TileStore* dst) {
  if (dst == nullptr || dst == &src) return false;
  if (roiIsEmpty(outRect)) return false;
  if (!fillParamsValid(p)) return false;

  // Colour is premultiplied once, up front: it does not vary per texel.
  const std::array<float, 4> colorPremultiplied{
      p.color[0] * p.opacity, p.color[1] * p.opacity, p.color[2] * p.opacity, p.opacity};

  // Gradient is evaluated once, into a scratch store with no selection --
  // this header's own comment on why `renderGradient()`'s `over` onto an
  // empty store is an exact identity for "the ramp's raw premultiplied
  // samples", nothing composited into yet.
  TileStore gradientRaw;
  if (p.source == FillSource::Gradient) {
    const GradientRegion region{outRect.x0, outRect.y0, outRect.x1, outRect.y1};
    renderGradient(gradientRaw, region, p.gradientGeometry, p.gradientStops, nullptr);
  }

  for (int32_t y = outRect.y0; y < outRect.y1; ++y) {
    for (int32_t x = outRect.x0; x < outRect.x1; ++x) {
      const PixelCoord doc{x, y};
      const TileCoord coord = tileCoordAt(doc);
      const PixelCoord local = tileLocalOffset(doc);

      std::array<float, 4> s{0.0f, 0.0f, 0.0f, 0.0f};
      switch (p.source) {
        case FillSource::Color:
          s = colorPremultiplied;
          break;
        case FillSource::Pattern: {
          const PixelCoord sc =
              patternSourceTexel(*p.pattern, p.patternOriginX, p.patternOriginY, doc);
          const float* q = p.pattern->px.data() +
                           (static_cast<size_t>(sc.y) * p.pattern->width +
                            static_cast<size_t>(sc.x)) *
                               4u;
          s = {q[0] * p.opacity, q[1] * p.opacity, q[2] * p.opacity, q[3] * p.opacity};
          break;
        }
        case FillSource::Gradient: {
          const Tile* t = gradientRaw.find(coord);
          const std::array<float, 4> g =
              t != nullptr ? t->readPixel(local) : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
          s = {g[0] * p.opacity, g[1] * p.opacity, g[2] * p.opacity, g[3] * p.opacity};
          break;
        }
      }

      const Tile* origTile = src.find(coord);
      const std::array<float, 4> o =
          origTile != nullptr ? origTile->readPixel(local) : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
      dst->getOrCreate(coord).writePixel(local, blendPixel(p.blend, s, o));
    }
  }
  return true;
}

}  // namespace np
