#include "brush/BrushModel.hpp"

namespace np {

const char* coverageBlendName(CoverageBlend blend) noexcept {
  switch (blend) {
    case CoverageBlend::Multiply: return "Multiply";
    case CoverageBlend::Overlay: return "Overlay";
    case CoverageBlend::ColorBurn: return "Color Burn";
    case CoverageBlend::HardMix: return "Hard Mix";
    case CoverageBlend::LinearBurn: return "Linear Burn";
    case CoverageBlend::ColorDodge: return "Color Dodge";
    case CoverageBlend::Darken: return "Darken";
    case CoverageBlend::Subtract: return "Subtract";
    case CoverageBlend::Height: return "Height";
    case CoverageBlend::LinearHeight: return "Linear Height";
  }
  return "unknown blend";
}

}  // namespace np
