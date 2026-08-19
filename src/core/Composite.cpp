#include "core/Composite.hpp"

#include <optional>

#include "core/Tile.hpp"
#include "core/TileStore.hpp"

namespace np {

float layerCoverage(const Layer& layer) noexcept {
  if (!layer.visible) return 0.0f;
  const float o = layer.opacity;
  if (!(o > 0.0f)) return 0.0f;  // also catches NaN
  return o < 1.0f ? o : 1.0f;
}

std::string unimplementedBlendWarning(size_t layerIndex, const Layer& layer) {
  std::string s = "layer " + std::to_string(layerIndex);
  if (!layer.name.empty()) s += " (\"" + layer.name + "\")";
  s += " asks for blend mode \"" + layer.blend + "\", which this build cannot composite. ";

  // The two cases are worth distinguishing in the sentence, because what a
  // user should do about them differs: an unknown name came from a newer
  // build and means "this build is behind the document", while `mix` means
  // "this build is behind PLAN.md" and has a named, dated unblocking
  // condition.
  if (blendModeFromName(layer.blend).has_value()) {
    s += "It is a mode core/Blend knows by name, but `Mix` is a Kubelka-Munk lerp between two "
         "pigment *latents* and no layer stores a latent yet -- Pigment layers own no tile "
         "storage until PLAN.md Phase 5 step 3. ";
  } else {
    s += "It is not one of the modes core/Blend implements (";
    bool first = true;
    for (const BlendModeInfo& info : allBlendModes()) {
      if (!info.compositesPixels) continue;
      if (!first) s += ", ";
      s += info.name;
      first = false;
    }
    s += "), so it most likely comes from a newer build. ";
  }

  s += "It was composited as \"";
  s += kDefaultBlendName;
  s += "\" (source-over) instead, so the composite is an approximation of what the layer stack "
       "means. The value itself is preserved exactly and is written back unchanged (PRD I10).";
  return s;
}

std::vector<float> compositeDocumentPremultiplied(const Document& doc,
                                                  std::vector<std::string>* warningsOut) {
  if (doc.width <= 0 || doc.height <= 0) return {};

  const size_t w = static_cast<size_t>(doc.width);
  const size_t h = static_cast<size_t>(doc.height);

  // Zero-filled: an untouched pixel is transparent black, exactly what
  // core::Tile gives an unwritten texel, so nothing needs a separate "was
  // anything here" flag -- and, per core/Blend.hpp's transparent-backdrop
  // identity, an all-zero accumulator composites the first contributing layer
  // through unchanged **under every mode**, not only under `over`.
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

    // Resolved once per layer, never per texel: `blend` is a std::string and a
    // string comparison in the inner loop would be the one plausible way to
    // make this walk slow. A name this build does not implement -- unknown, or
    // `mix` -- becomes `Normal`, which is precisely the approximation the
    // warning above describes, made in one place rather than at each texel.
    const std::optional<BlendMode> named = blendModeFromName(layer.blend);
    const BlendMode mode = (named.has_value() && blendModeInfo(*named).compositesPixels)
                               ? *named
                               : BlendMode::Normal;

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
              blendPixel(mode, src, {dst[0], dst[1], dst[2], dst[3]});
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
