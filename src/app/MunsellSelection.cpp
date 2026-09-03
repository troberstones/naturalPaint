#include "app/MunsellSelection.hpp"

#include "color/Munsell.hpp"
#include "color/Space.hpp"

#include <algorithm>
#include <cmath>

namespace np {

double munsellRowLStar(int row, int steps) noexcept {
  const double v = munsellPageRowValue(row, steps);
  return lStarFromRelativeLuminance(munsellValueToLuminanceFactor(v) / 100.0);
}

double munsellPageChromaFor(const BrushState& brush) noexcept {
  return pageChroma(brush.munsellSteps, static_cast<double>(brush.munsellHueDeg));
}

double munsellCellChroma(const BrushState& brush, int row, int col) noexcept {
  const int n = brush.munsellSteps;
  if (n <= 1) return 0.0;
  const double span = brush.munsellPerRowChroma
                          ? maxInGamutChroma(munsellRowLStar(row, n),
                                             static_cast<double>(brush.munsellHueDeg))
                          : munsellPageChromaFor(brush);
  return static_cast<double>(col) * span / static_cast<double>(n - 1);
}

std::optional<std::array<float, 3>> munsellCellSrgb(const BrushState& brush, int row,
                                                    int col) noexcept {
  const auto linear = munsellCellLinearRgb(munsellRowLStar(row, brush.munsellSteps),
                                           munsellCellChroma(brush, row, col),
                                           static_cast<double>(brush.munsellHueDeg));
  if (!linear) return std::nullopt;
  // **The encode, and the only one in this feature.** `color/Munsell` answers
  // linear light; `BrushState::rgb` is display-referred sRGB (app/AppState.hpp).
  // Storing the linear triple here would land every Munsell stroke roughly
  // twice as dark as the chip that was clicked -- the failure that field's own
  // header warns reads as "colour management is broken somewhere else".
  return std::array<float, 3>{srgbEncode((*linear)[0]), srgbEncode((*linear)[1]),
                              srgbEncode((*linear)[2])};
}

void clampMunsellSelection(BrushState& brush) noexcept {
  brush.munsellSteps = std::clamp(brush.munsellSteps, kMinPageSteps, kMaxPageSteps);
  const int n = brush.munsellSteps;
  brush.munsellRow = std::clamp(brush.munsellRow, 0, n - 1);
  brush.munsellCol = std::clamp(brush.munsellCol, 0, n - 1);
  // Hue is a circle, so it wraps rather than clamping -- a picker whose hue
  // strip stopped at 359 and refused to continue would be a bug, not a limit.
  float h = std::fmod(brush.munsellHueDeg, 360.0f);
  if (h < 0.0f) h += 360.0f;
  brush.munsellHueDeg = h;

  // Column 0 is chroma 0, the row's neutral, and it is in gamut for every row
  // of every page -- so this walk always terminates on a live cell and the
  // `while` needs no separate guard.
  while (brush.munsellCol > 0 && !munsellCellSrgb(brush, brush.munsellRow, brush.munsellCol))
    --brush.munsellCol;
}

void applyMunsellSelection(BrushState& brush) noexcept {
  clampMunsellSelection(brush);
  if (const auto srgb = munsellCellSrgb(brush, brush.munsellRow, brush.munsellCol))
    brush.rgb = *srgb;
}

}  // namespace np
