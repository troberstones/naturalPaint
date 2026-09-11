#include "core/Region.hpp"

namespace np {

const char* regionKindName(RegionKind kind) noexcept {
  switch (kind) {
    case RegionKind::Frame: return "Frame";
    case RegionKind::Slice: return "Slice";
  }
  return "?";
}

}  // namespace np
