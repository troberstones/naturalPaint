#include "brush/BrushModelDiff.hpp"

namespace np {

std::vector<std::string> brushModelDiff(const BrushModel& a, const BrushModel& b) {
  std::vector<std::string> out;
  // `==` on every leaf, EXACT rather than within a tolerance. This function
  // answers "did the user change it" and "did a round trip survive", and
  // both questions are about the bytes as saved, not about whether they are
  // close: a tolerance would make a genuine edit of 0.0000001 invisible to
  // the EDITED badge, and would make a lossy round trip report success.
  auto visit = [&](const std::string& path, const auto& av, const auto& bv) -> bool {
    if (!(av == bv)) out.push_back(path);
    return true;  // every mismatch is wanted here, not just the first
  };
  detail::visitBrushModel(a, b, visit);
  return out;
}

bool brushModelEqual(const BrushModel& a, const BrushModel& b) noexcept {
  bool equal = true;
  auto visit = [&](const std::string&, const auto& av, const auto& bv) -> bool {
    if (!(av == bv)) {
      equal = false;
      return false;  // stop walking -- this is the "cheaper" half of the pair
    }
    return true;
  };
  detail::visitBrushModel(a, b, visit);
  return equal;
}

std::vector<std::string> brushModelDiffPaths() {
  std::vector<std::string> out;
  // Any single model names every path -- the walk never reads a value here,
  // only the leaf it is standing at, so `m` is compared against itself
  // purely to give the visitor two same-typed arguments to deduce from.
  const BrushModel m;
  auto visit = [&](const std::string& path, const auto&, const auto&) -> bool {
    out.push_back(path);
    return true;
  };
  detail::visitBrushModel(m, m, visit);
  return out;
}

}  // namespace np
