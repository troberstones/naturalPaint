#include "app/Snapping.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace np {

std::vector<float> gridLinePositions(float spacing, int subdivisions, float rangeMin,
                                     float rangeMax) {
  std::vector<float> out;
  if (!(spacing > 0.0f) || subdivisions < 1 || rangeMax < rangeMin) return out;
  const float minor = spacing / static_cast<float>(subdivisions);
  if (!(minor > 0.0f)) return out;

  // A small tolerance so a range boundary that lands exactly on a grid line
  // (the common case -- e.g. [0,100]) includes that line rather than
  // dropping it to floating-point rounding.
  const float eps = minor * 1e-4f;
  const auto startIdx = static_cast<int64_t>(std::ceil((rangeMin - eps) / minor));
  const auto endIdx = static_cast<int64_t>(std::floor((rangeMax + eps) / minor));
  if (endIdx < startIdx) return out;

  out.reserve(static_cast<size_t>(endIdx - startIdx + 1));
  for (int64_t i = startIdx; i <= endIdx; ++i) out.push_back(static_cast<float>(i) * minor);
  return out;
}

bool isMajorGridLine(float pos, float spacing) {
  if (!(spacing > 0.0f)) return false;
  const float r = std::fmod(std::fabs(pos), spacing);
  const float eps = spacing * 1e-3f;
  return r < eps || (spacing - r) < eps;
}

std::optional<float> parseGuidePosition(std::string_view text, float axisExtent) {
  size_t b = 0, e = text.size();
  while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
  text = text.substr(b, e - b);
  if (text.empty()) return std::nullopt;

  const bool percent = text.back() == '%';
  const std::string numPart(percent ? text.substr(0, text.size() - 1) : text);
  if (numPart.empty()) return std::nullopt;

  char* end = nullptr;
  const float value = std::strtof(numPart.c_str(), &end);
  if (end == numPart.c_str() || *end != '\0') return std::nullopt;

  return percent ? (value * 0.01f * axisExtent) : value;
}

SnapResult resolveSnap(Vec2 point, const std::vector<Guide>& guides, float gridSpacing,
                       int gridSubdivisions, float canvasW, float canvasH,
                       float thresholdDoc) {
  SnapResult result{point, false, false};
  if (!(thresholdDoc > 0.0f)) return result;

  float bestX = point.x, bestXDist = thresholdDoc;
  float bestY = point.y, bestYDist = thresholdDoc;
  bool haveX = false, haveY = false;

  auto considerX = [&](float candidate) {
    const float d = std::fabs(candidate - point.x);
    if (d <= bestXDist) {
      bestXDist = d;
      bestX = candidate;
      haveX = true;
    }
  };
  auto considerY = [&](float candidate) {
    const float d = std::fabs(candidate - point.y);
    if (d <= bestYDist) {
      bestYDist = d;
      bestY = candidate;
      haveY = true;
    }
  };

  // Lowest priority first, guides last -- see this header's doc comment on
  // why: candidates are accepted on "<=" so a later, equal-distance category
  // overrides an earlier one, which is what makes guides win ties.
  if (gridSpacing > 0.0f && gridSubdivisions >= 1) {
    const float minor = gridSpacing / static_cast<float>(gridSubdivisions);
    if (minor > 0.0f) {
      considerX(std::round(point.x / minor) * minor);
      considerY(std::round(point.y / minor) * minor);
    }
  }

  considerX(0.0f);
  considerX(canvasW);
  considerY(0.0f);
  considerY(canvasH);

  for (const auto& g : guides) {
    if (g.orientation == GuideOrientation::Vertical) considerX(g.position);
    else considerY(g.position);
  }

  result.point.x = haveX ? bestX : point.x;
  result.point.y = haveY ? bestY : point.y;
  result.snappedX = haveX;
  result.snappedY = haveY;
  return result;
}

}  // namespace np
