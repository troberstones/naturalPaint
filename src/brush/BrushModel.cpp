#include "brush/BrushModel.hpp"

#include <algorithm>

namespace np {

// `coverageBlendName()` lives in brush/CoverageBlend.cpp -- this file owns
// the model, not the blend arithmetic.

std::shared_ptr<const BrushTip> dualTipFromModel(const PsDualBrush& dual) {
  if (!dual.enabled || !coverageBlendIsRenderable(dual.blend)) return nullptr;
  auto tip = std::make_shared<BrushTip>();
  tip->radius = std::clamp(dual.tip.diameterPx * 0.5f, 0.5f, 4096.0f);
  // Hard whatever the file's second tip says: Photoshop's Dual Brush panel has
  // no Hardness control, and the import has always stamped the second tip hard.
  tip->hardness = 1.0f;
  tip->roundness = std::clamp(dual.tip.roundness, 0.01f, 1.0f);
  tip->angle = dual.tip.angleDeg;
  // `spacingPercent` is a percentage of the diameter; `BrushTip::spacing` is in
  // radii, and a diameter is two of them. Clamped as `abrSpacingToRadii()` is:
  // a zero spacing is an unbounded dab count.
  tip->spacing = std::clamp(dual.tip.spacingPercent / 100.0f * 2.0f, 0.02f, 8.0f);
  tip->bitmap = dual.tip.dab.bitmap;
  return tip;
}

bool grainFromTexture(const PsTexture& texture, GrainParams& grain, std::string* why) {
  const auto refuse = [why](std::string reason) {
    if (why != nullptr) *why = std::move(reason);
    return false;
  };
  if (!texture.enabled) return refuse("Texture is off");
  if (texture.pattern.id.empty()) return refuse("Texture is on but names no pattern");
  if (texture.pattern.field == nullptr)
    return refuse("Texture names pattern '" + texture.pattern.name + "', which is not loaded");
  if (!coverageBlendIsRenderable(texture.blend))
    return refuse(std::string("Texture's blend mode '") + coverageBlendName(texture.blend) +
                  "' has no per-pixel formula in any source consulted");

  grain.enabled = true;
  grain.field = texture.pattern.field;
  grain.depth = std::clamp(texture.depth, 0.0f, 1.0f);
  // Photoshop's Scale is a percentage of the pattern's own size. Clamped away
  // from zero because a zero scale is a division, and at the top because a
  // pattern stretched a hundredfold is a flat colour, not paper.
  grain.scale = std::clamp(texture.scalePercent / 100.0f, 0.01f, 16.0f);
  grain.invert = texture.invert;
  // Brightness in 8-bit levels: its -150..150 range is Photoshop's Brightness/
  // Contrast adjustment's (INFERRED for the Texture panel). Read as hundredths,
  // Brightness -50 blanked Perfect Pencil Basic and Dry Brush Linework.
  grain.brightness = std::clamp(texture.brightness / 255.0f, -1.0f, 1.0f);
  grain.contrast = std::clamp(texture.contrast / 100.0f, -1.0f, 1.0f);
  grain.blend = texture.blend;
  // `strength` stays at its default 1.0: Photoshop's Texture panel has no
  // second multiplier on the tip's coverage, so inventing one from `depth`
  // would be an opinion rather than the file's.
  return true;
}

}  // namespace np
