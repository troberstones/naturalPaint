#include "brush/NativeBrush.hpp"

namespace np {
namespace {

bool stabiliserSettingEqual(const BrushStabiliserSetting& a,
                            const BrushStabiliserSetting& b) noexcept {
  if (a.mode != b.mode || a.amountPct != b.amountPct) return false;
  if (a.mode != StabiliserBrushMode::Own) return true;  // `own` unread otherwise
  const StabiliserParams& x = a.own;
  const StabiliserParams& y = b.own;
  return x.mode == y.mode && x.stringPx == y.stringPx && x.strength == y.strength &&
         x.responsiveness == y.responsiveness && x.catchUpAtEnd == y.catchUpAtEnd &&
         x.catchUpWhilePaused == y.catchUpWhilePaused &&
         x.stabilisePressure == y.stabilisePressure && x.scaleWithZoom == y.scaleWithZoom &&
         x.showString == y.showString;
}

}  // namespace

bool nativeBrushEqual(const NativeBrush& a, const NativeBrush& b) noexcept {
  return a.load == b.load && a.wetness == b.wetness && grainParamsEqual(a.grain, b.grain) &&
         stabiliserSettingEqual(a.stabiliser, b.stabiliser) && a.taperInPx == b.taperInPx &&
         a.taperMinSize == b.taperMinSize && a.taperFlow == b.taperFlow;
}

}  // namespace np
