#include "core/RegionOps.hpp"

#include "core/LayerOpRefusal.hpp"

namespace np {
namespace {

bool regionInRange(const Document& doc, size_t index, const char* what, LayerOpResult* out) {
  if (index < doc.regions.size()) return true;
  *out = layerOpFail(std::string(what) + " refused: there is no region at index " +
                     std::to_string(index) + " -- this document has " +
                     std::to_string(doc.regions.size()) + " region(s).");
  return false;
}

std::string describeRegion(const Document& doc, size_t index) {
  const Region& r = doc.regions[index];
  return std::string(regionKindName(r.kind)) + " \"" + r.name + "\"";
}

}  // namespace

std::string uniqueRegionName(const Document& doc, std::string_view desired,
                             size_t excludeIndex) {
  const std::string base(desired);
  const auto collides = [&](const std::string& candidate) {
    for (size_t i = 0; i < doc.regions.size(); ++i) {
      if (i == excludeIndex) continue;
      if (doc.regions[i].name == candidate) return true;
    }
    return false;
  };
  if (!collides(base)) return base;
  // `core::uniqueChannelName()`'s own search: the smallest free " N" suffix
  // from 2 up, rather than "highest used plus one" -- a document that once
  // had "Frame 1".."Frame 5" and deleted the middle three should offer
  // "Frame 1 2", not "Frame 1 6", to whoever asks for "Frame 1" next. (In
  // practice `addRegion()` never asks with a colliding *default* name --
  // `defaultNewRegionName()` already avoids that -- so this branch is really
  // for a user-typed rename that collides.)
  for (size_t n = 2;; ++n) {
    std::string candidate = base + " " + std::to_string(n);
    if (!collides(candidate)) return candidate;
  }
}

std::string defaultNewRegionName(const Document& doc, RegionKind kind) {
  const std::string stem = regionKindName(kind);
  const std::string prefix = stem + " ";
  long highest = 0;
  for (const Region& r : doc.regions) {
    if (r.name.rfind(prefix, 0) != 0) continue;
    const std::string digits = r.name.substr(prefix.size());
    if (digits.empty()) continue;
    bool allDigits = true;
    for (const char c : digits)
      if (c < '0' || c > '9') allDigits = false;
    if (!allDigits) continue;
    long value = 0;
    for (const char c : digits) {
      value = value * 10 + (c - '0');
      if (value > 1000000) break;  // saturate rather than overflow
    }
    if (value > highest) highest = value;
  }
  return uniqueRegionName(doc, prefix + std::to_string(highest + 1));
}

LayerOpResult addRegion(Document& doc, RegionKind kind, int32_t x, int32_t y, uint32_t width,
                        uint32_t height, std::string name) {
  if (width == 0u || height == 0u) {
    return layerOpFail(std::string("add ") + regionKindName(kind) +
                       " refused: the requested rectangle is " + std::to_string(width) + "x" +
                       std::to_string(height) +
                       ", and a region with a zero dimension is nothing a user could see, drag "
                       "or export. Nothing was added.");
  }

  Region region;
  region.id = doc.nextRegionId++;
  region.kind = kind;
  region.name = name.empty() ? defaultNewRegionName(doc, kind) : uniqueRegionName(doc, name);
  region.x = x;
  region.y = y;
  region.width = width;
  region.height = height;

  doc.regions.push_back(std::move(region));
  const size_t index = doc.regions.size() - 1;
  return layerOpSucceed("add " + describeRegion(doc, index), index);
}

LayerOpResult deleteRegion(Document& doc, size_t index) {
  LayerOpResult refusal;
  if (!regionInRange(doc, index, "delete region", &refusal)) return refusal;
  const std::string label = "delete " + describeRegion(doc, index);
  doc.regions.erase(doc.regions.begin() + static_cast<std::ptrdiff_t>(index));
  return layerOpSucceed(label, index);
}

LayerOpResult renameRegion(Document& doc, size_t index, std::string name) {
  LayerOpResult refusal;
  if (!regionInRange(doc, index, "rename region", &refusal)) return refusal;
  const std::string oldLabel = describeRegion(doc, index);
  doc.regions[index].name = uniqueRegionName(doc, name, index);
  return layerOpSucceed("rename " + oldLabel + " to \"" + doc.regions[index].name + "\"", index);
}

LayerOpResult moveRegion(Document& doc, size_t index, int32_t x, int32_t y) {
  LayerOpResult refusal;
  if (!regionInRange(doc, index, "move region", &refusal)) return refusal;
  const std::string label = "move " + describeRegion(doc, index);
  doc.regions[index].x = x;
  doc.regions[index].y = y;
  return layerOpSucceed(label, index);
}

LayerOpResult resizeRegion(Document& doc, size_t index, int32_t x, int32_t y, uint32_t width,
                           uint32_t height) {
  LayerOpResult refusal;
  if (!regionInRange(doc, index, "resize region", &refusal)) return refusal;
  if (width == 0u || height == 0u) {
    return layerOpFail("resize " + describeRegion(doc, index) + " refused: the requested "
                       "rectangle is " + std::to_string(width) + "x" + std::to_string(height) +
                       ". Nothing was changed.");
  }
  const std::string label = "resize " + describeRegion(doc, index);
  doc.regions[index].x = x;
  doc.regions[index].y = y;
  doc.regions[index].width = width;
  doc.regions[index].height = height;
  return layerOpSucceed(label, index);
}

}  // namespace np
