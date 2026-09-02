#include "app/GradientTool.hpp"

#include <type_traits>

namespace np {

// The table § 4 of the header argues for. Order is the order the combo lists
// them, and it is deliberately the enum's own order so a reader comparing
// this against `ops/Gradient.hpp` can do it line by line.
const GradientSpreadRow kGradientSpreads[kGradientSpreadCount] = {
    {GradientSpread::Pad, "Clamp",
     "Outside the drag, the ramp's two end colours are held -- everything behind the start "
     "handle takes the first stop, everything past the end handle takes the last. The "
     "default, and the only mode that cannot surprise a short drag on a big canvas."},
    {GradientSpread::Repeat, "Repeat",
     "The ramp tiles outside the drag, restarting from the first stop each time. A hard seam "
     "wherever the last stop meets the first again, which is the point when the ramp is a "
     "stripe pattern and a defect when it is not."},
    {GradientSpread::Reflect, "Reflect",
     "The ramp tiles outside the drag, mirrored on every other repeat, so the last stop meets "
     "itself and there is no seam. Repeat's answer when the ramp's two ends do not match."},
};

// The count is not enough on its own -- `kToolMeta` (ui/AtelierChrome.cpp) is
// checked by exactly this kind of assert and still shipped rows in the wrong
// order, because a COUNT check passes on any permutation and on any duplicate.
// So `--selftest` walks the enum and asserts each value appears here exactly
// once; this assert is only the cheap half that fires at compile time.
static_assert(kGradientSpreadCount == 3,
              "kGradientSpreads must carry every GradientSpread; see the enum in "
              "ops/Gradient.hpp and app/selftest/GradientTool.cpp's coverage assertion");

const char* gradientSpreadLabel(GradientSpread spread) {
  for (size_t i = 0; i < kGradientSpreadCount; ++i)
    if (kGradientSpreads[i].spread == spread) return kGradientSpreads[i].label;
  return "Clamp";
}

GradientStops gradientToolStops(const std::array<float, 4>& foregroundLinear) {
  GradientStops stops;
  // The midpoint on every stop is 0.5 -- the linear interpolation
  // `gradientParameterAt()` degenerates to when the control is centred. This
  // build surfaces no midpoint control, so writing anything else here would
  // be a bias nothing in the UI could explain or undo.
  const float r = foregroundLinear[0];
  const float g = foregroundLinear[1];
  const float b = foregroundLinear[2];
  stops.colorStops.push_back(ColorStop{0.0f, {r, g, b}, 0.5f});
  stops.colorStops.push_back(ColorStop{1.0f, {r, g, b}, 0.5f});
  stops.opacityStops.push_back(OpacityStop{0.0f, 1.0f, 0.5f});
  stops.opacityStops.push_back(OpacityStop{1.0f, 0.0f, 0.5f});
  return stops;
}

GradientGeometry gradientToolGeometry(const GradientToolState& tool, float x0, float y0,
                                      float x1, float y1) {
  GradientGeometry geom;
  // Hard-coded, and honestly so: the palette offers one gradient tool and the
  // options bar offers no kind picker, so Linear is not a default here -- it
  // is the only shape this tool can currently aim. `ops/Gradient` implements
  // Radial and angular already; wiring them is a combo beside SPREAD and a
  // second field on `GradientToolState`, and belongs with whatever change
  // adds that combo rather than as an unreachable parameter added early.
  geom.kind = GradientKind::Linear;
  geom.x0 = x0;
  geom.y0 = y0;
  geom.x1 = x1;
  geom.y1 = y1;
  geom.spread = tool.spread;
  return geom;
}

bool gradientDragIsUsable(float x0, float y0, float x1, float y1) {
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  return dx * dx + dy * dy >= 1.0f;
}

}  // namespace np
