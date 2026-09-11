#include "brush/NativeBrush.hpp"

namespace np {

bool nativeBrushEqual(const NativeBrush& a, const NativeBrush& b) noexcept {
  return a.load == b.load && a.wetness == b.wetness && grainParamsEqual(a.grain, b.grain);
}

}  // namespace np
