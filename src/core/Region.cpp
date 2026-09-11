#include "core/Region.hpp"

namespace np {

const char* regionKindName(RegionKind kind) noexcept {
  switch (kind) {
    case RegionKind::Frame: return "Frame";
    case RegionKind::Slice: return "Slice";
  }
  return "?";
}

std::optional<RegionKind> regionKindFromName(std::string_view name) noexcept {
  if (name == "Frame") return RegionKind::Frame;
  if (name == "Slice") return RegionKind::Slice;
  return std::nullopt;
}

}  // namespace np
