#include "core/Composite.hpp"

#include "core/Tile.hpp"
#include "core/TileStore.hpp"

namespace np {

std::array<float, 4> compositeOver(const std::array<float, 4>& src,
                                   const std::array<float, 4>& dst) noexcept {
  const float inv = 1.0f - src[3];
  return {src[0] + dst[0] * inv, src[1] + dst[1] * inv, src[2] + dst[2] * inv,
          src[3] + dst[3] * inv};
}

float layerCoverage(const Layer& layer) noexcept {
  if (!layer.visible) return 0.0f;
  const float o = layer.opacity;
  if (!(o > 0.0f)) return 0.0f;  // also catches NaN
  return o < 1.0f ? o : 1.0f;
}

bool blendIsImplemented(std::string_view blend) noexcept {
  return blend == kDefaultBlendName;
}

std::string unimplementedBlendWarning(size_t layerIndex, const Layer& layer) {
  std::string s = "layer " + std::to_string(layerIndex);
  if (!layer.name.empty()) s += " (\"" + layer.name + "\")";
  s += " asks for blend mode \"" + layer.blend +
       "\", which this build does not implement -- the only blend implemented here is \"" +
       kDefaultBlendName +
       "\" (source-over). It was composited as \"" + kDefaultBlendName +
       "\" instead, so the composite is an approximation of what the layer stack means. The "
       "value itself is preserved exactly and is written back unchanged (PRD I10); the linear-"
       "safe blend set is PLAN.md Phase 5 step 2 (core/Blend).";
  return s;
}

std::vector<float> compositeDocumentPremultiplied(const Document& doc,
                                                  std::vector<std::string>* warningsOut) {
  if (doc.width <= 0 || doc.height <= 0) return {};

  const size_t w = static_cast<size_t>(doc.width);
  const size_t h = static_cast<size_t>(doc.height);

  // Zero-filled: an untouched pixel is transparent black, exactly what
  // core::Tile gives an unwritten texel, so nothing needs a separate "was
  // anything here" flag -- and, per compositeOver()'s identity, an all-zero
  // accumulator composites the first contributing layer through unchanged.
  std::vector<float> out(w * h * 4, 0.0f);

  // Bottom to top: index 0 first, so each layer is `src` over everything
  // already accumulated beneath it. See the header on why this direction is
  // the file format's and not a free choice.
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    const Layer& layer = doc.layers[i];
    if (layer.kind != LayerKind::RGB || !layer.rgbTiles.has_value()) continue;

    // Warned about even when the layer contributes nothing, and warned about
    // before the coverage test: whether the composite is an approximation is a
    // property of the document, not of whether this particular hidden layer
    // happened to matter today. A user who unhides it must not discover the
    // approximation only then.
    if (!blendIsImplemented(layer.blend) && warningsOut != nullptr)
      warningsOut->push_back(unimplementedBlendWarning(i, layer));

    const float coverage = layerCoverage(layer);
    // A hidden layer, or one at zero opacity, contributes **exactly** nothing:
    // this is a skip, not a multiply by zero that could still perturb the
    // accumulator through a signed zero or a NaN texel. --selftest asserts the
    // hidden case at zero tolerance.
    if (coverage <= 0.0f) continue;

    for (const auto& [coord, tile] : *layer.rgbTiles) {
      const PixelCoord origin = tileOrigin(coord);
      for (int32_t ty = 0; ty < kTileSize; ++ty) {
        const int32_t docY = origin.y + ty;
        if (docY < 0 || docY >= doc.height) continue;  // clipped to the canvas
        for (int32_t tx = 0; tx < kTileSize; ++tx) {
          const int32_t docX = origin.x + tx;
          if (docX < 0 || docX >= doc.width) continue;

          std::array<float, 4> src = tile.readPixel(PixelCoord{tx, ty});
          // The one place opacity enters. At the default 1.0 this is a
          // multiplication by literal 1.0f -- exact for every finite float --
          // which is half of why an unchanged document composites
          // bit-identically to the plain sum this replaced.
          if (coverage != 1.0f) {
            src[0] *= coverage;
            src[1] *= coverage;
            src[2] *= coverage;
            src[3] *= coverage;
          }

          float* dst = &out[(static_cast<size_t>(docY) * w + static_cast<size_t>(docX)) * 4];
          const std::array<float, 4> composited =
              compositeOver(src, {dst[0], dst[1], dst[2], dst[3]});
          dst[0] = composited[0];
          dst[1] = composited[1];
          dst[2] = composited[2];
          dst[3] = composited[3];
        }
      }
    }
  }

  return out;
}

}  // namespace np
