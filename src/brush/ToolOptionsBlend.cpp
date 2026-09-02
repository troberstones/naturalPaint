#include "brush/ToolOptionsBlend.hpp"

namespace np {

bool blendModeFromPsToolOptions(const std::string& psId, BlendMode& out,
                                std::string* reasonOut) {
  if (psId.empty() || psId == "Nrml") {
    out = BlendMode::Normal;
    return true;
  }
  if (psId == "Mltp") {
    out = BlendMode::Multiply;
    return true;
  }
  // See this header's own comment: the same Darken-is-Min equivalence
  // brush/CoverageBlend.cpp's `applyCoverageBlend()` already uses for the
  // Dual Brush/Texture table, extended componentwise to premultiplied RGBA.
  if (psId == "Drkn") {
    out = BlendMode::Min;
    return true;
  }

  if (reasonOut != nullptr) {
    if (psId == "linearBurn") {
      *reasonOut =
          "\"linearBurn\" (Linear Burn) has no naturalPaint per-stroke blend implementation -- "
          "it would need a new core::BlendMode this task deliberately does not add.";
    } else if (psId == "Dslv") {
      *reasonOut =
          "\"Dslv\" (Dissolve) has no naturalPaint per-stroke blend implementation -- it is a "
          "per-pixel random threshold, not a deterministic two-colour blend, and has no formula "
          "in core/Blend.hpp.";
    } else {
      *reasonOut = "\"" + psId + "\" is not a blend mode id this build recognises.";
    }
  }
  return false;
}

}  // namespace np
