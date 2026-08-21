#include "core/DirtyTiles.hpp"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <unordered_set>

#include "core/Mask.hpp"
#include "core/Pigment.hpp"
#include "core/TileStore.hpp"

namespace np {
namespace {

// Bitwise, deliberately, and on the POD params structs only. `==` on floats
// would report a NaN param as changed on every single frame -- conservative,
// but a document carrying one would never take the incremental path again.
// Bitwise equality is the honest question here: identical bits grade
// identically.
template <class T>
bool sameBits(const T& a, const T& b) noexcept {
  static_assert(std::is_trivially_copyable_v<T>, "sameBits is for POD params only");
  return std::memcmp(&a, &b, sizeof(T)) == 0;
}

bool curvesEqual(const std::array<Curve, 3>& a, const std::array<Curve, 3>& b) {
  for (size_t c = 0; c < 3; ++c) {
    if (a[c].size() != b[c].size()) return false;
    if (a[c].empty()) continue;
    if (std::memcmp(a[c].data(), b[c].data(), a[c].size() * sizeof(CurvePoint)) != 0) return false;
  }
  return true;
}

bool opsEqual(const Op& a, const Op& b) {
  if (a.opClass != b.opClass || a.enabled != b.enabled || a.pointKind != b.pointKind) return false;
  if (!sameBits(a.levels, b.levels)) return false;
  if (!curvesEqual(a.curves, b.curves)) return false;
  if (!sameBits(a.exposure, b.exposure)) return false;
  if (!sameBits(a.saturation, b.saturation)) return false;
  if (!sameBits(a.grayscale, b.grayscale)) return false;
  if (!sameBits(a.channelMixer, b.channelMixer)) return false;
  return a.unrecognised == b.unrecognised;
}

// Structural, not `OpStack::version()`. See core/DirtyTiles.hpp §3 on why a
// version is a complete detector for one stack over time and not across a
// reorder.
bool opStacksEqual(const OpStack& a, const OpStack& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (!opsEqual(a.at(i), b.at(i))) return false;
  return true;
}

// The whole of the tile-identity diff, for one pair of stores of any tile
// type. A null store is "no store at all", which is not the same thing as an
// empty one -- but for the *contents* diff the two behave identically, and
// the presence difference is caught separately by the caller (it changes
// which branch of the compositor runs, not merely which tiles exist).
template <class Store>
void diffStore(const Store* before, const Store* after, std::unordered_set<TileCoord>& dirty) {
  if (after != nullptr) {
    for (const auto& [coord, tile] : *after) {
      // `find()` returns the slot's address, which is exactly the identity
      // core/TileStore's barrier changes on a write to a shared tile.
      const auto* was = before ? before->find(coord) : nullptr;
      if (was != &tile) dirty.insert(coord);
    }
  }
  if (before != nullptr) {
    for (const auto& [coord, tile] : *before) {
      (void)tile;
      if (after == nullptr || after->find(coord) == nullptr) dirty.insert(coord);
    }
  }
}

bool tileCoordLess(const TileCoord& a, const TileCoord& b) noexcept {
  return a.y != b.y ? a.y < b.y : a.x < b.x;
}

// Ceil-division of a positive canvas extent into tiles.
int32_t tilesAcross(int32_t extent) noexcept {
  return extent <= 0 ? 0 : (extent + kTileSize - 1) / kTileSize;
}

}  // namespace

const char* fullRecompositeReasonName(FullRecompositeReason reason) noexcept {
  switch (reason) {
    case FullRecompositeReason::None: return "none";
    case FullRecompositeReason::NoPreviousComposite: return "no previous composite";
    case FullRecompositeReason::CanvasSizeChanged: return "canvas size changed";
    case FullRecompositeReason::WorkingSpaceChanged: return "working space changed";
    case FullRecompositeReason::LayerCountChanged: return "layer count changed";
    case FullRecompositeReason::LayerKindChanged: return "layer kind changed";
    case FullRecompositeReason::LayerVisibilityChanged: return "layer visibility changed";
    case FullRecompositeReason::LayerOpacityChanged: return "layer opacity changed";
    case FullRecompositeReason::LayerBlendChanged: return "layer blend changed";
    case FullRecompositeReason::LayerClipChanged: return "layer clip changed";
    case FullRecompositeReason::LayerOpsChanged: return "layer op stack changed";
    case FullRecompositeReason::LayerMaskPresenceChanged: return "layer mask added or removed";
    case FullRecompositeReason::LayerStoragePresenceChanged: return "layer tile store added or "
                                                                    "removed";
  }
  return "?";
}

std::string fullRecompositeExplanation(FullRecompositeReason reason, size_t layerIndex) {
  if (reason == FullRecompositeReason::None) return {};
  std::string s = "the whole canvas was recomposited because ";
  s += fullRecompositeReasonName(reason);
  switch (reason) {
    case FullRecompositeReason::LayerKindChanged:
    case FullRecompositeReason::LayerVisibilityChanged:
    case FullRecompositeReason::LayerOpacityChanged:
    case FullRecompositeReason::LayerBlendChanged:
    case FullRecompositeReason::LayerClipChanged:
    case FullRecompositeReason::LayerOpsChanged:
    case FullRecompositeReason::LayerMaskPresenceChanged:
    case FullRecompositeReason::LayerStoragePresenceChanged:
      s += " on layer " + std::to_string(layerIndex);
      break;
    default: break;
  }
  s += ". That is not a tile-local change: it moves every texel the layer covers, and an "
       "adjustment layer's op stack moves every texel beneath it, so there is no rectangle "
       "small enough to be worth finding (core/DirtyTiles.hpp §3).";
  return s;
}

DocumentDirtyTiles documentDirtyTiles(const Document& before, const Document& after) {
  DocumentDirtyTiles out;
  out.layerIndex = after.layers.size();

  auto whole = [&](FullRecompositeReason reason, size_t layerIndex) {
    out.everything = true;
    out.reason = reason;
    out.layerIndex = layerIndex;
    out.tiles.clear();
    return out;
  };

  if (before.width != after.width || before.height != after.height)
    return whole(FullRecompositeReason::CanvasSizeChanged, after.layers.size());
  if (!sameBits(before.workingSpace.primaries, after.workingSpace.primaries))
    return whole(FullRecompositeReason::WorkingSpaceChanged, after.layers.size());
  if (before.layers.size() != after.layers.size())
    return whole(FullRecompositeReason::LayerCountChanged, after.layers.size());

  // Every compared property first, for every layer, before any tile is
  // touched: a property change makes the tile diff irrelevant, and doing the
  // cheap pass first means a visibility toggle never walks a store at all.
  for (size_t i = 0; i < after.layers.size(); ++i) {
    const Layer& a = before.layers[i];
    const Layer& b = after.layers[i];
    if (a.kind != b.kind) return whole(FullRecompositeReason::LayerKindChanged, i);
    if (a.visible != b.visible) return whole(FullRecompositeReason::LayerVisibilityChanged, i);
    // Bitwise, for the reason `sameBits()` gives: a NaN opacity is clamped to
    // 0 by `layerCoverage()` and must not be reported as changing every frame.
    if (!sameBits(a.opacity, b.opacity))
      return whole(FullRecompositeReason::LayerOpacityChanged, i);
    if (a.blend != b.blend) return whole(FullRecompositeReason::LayerBlendChanged, i);
    if (a.clipped != b.clipped) return whole(FullRecompositeReason::LayerClipChanged, i);
    if (!opStacksEqual(a.ops, b.ops)) return whole(FullRecompositeReason::LayerOpsChanged, i);
    if (a.mask.has_value() != b.mask.has_value())
      return whole(FullRecompositeReason::LayerMaskPresenceChanged, i);
    if (a.rgbTiles.has_value() != b.rgbTiles.has_value() ||
        a.pigmentTiles.has_value() != b.pigmentTiles.has_value())
      return whole(FullRecompositeReason::LayerStoragePresenceChanged, i);
  }

  std::unordered_set<TileCoord> dirty;
  for (size_t i = 0; i < after.layers.size(); ++i) {
    const Layer& a = before.layers[i];
    const Layer& b = after.layers[i];
    diffStore(a.rgbTiles ? &*a.rgbTiles : nullptr, b.rgbTiles ? &*b.rgbTiles : nullptr, dirty);
    diffStore(a.pigmentTiles ? &*a.pigmentTiles : nullptr,
              b.pigmentTiles ? &*b.pigmentTiles : nullptr, dirty);
    diffStore(a.mask ? &*a.mask : nullptr, b.mask ? &*b.mask : nullptr, dirty);
  }

  out.tiles.assign(dirty.begin(), dirty.end());
  // Sorted so that two runs over the same edit produce the same set in the
  // same order -- which is what makes the upload's per-tile `writeTexture`
  // calls reproducible, and what lets `--selftest` compare tile lists rather
  // than only their sizes. `std::unordered_set` iteration order is not.
  std::sort(out.tiles.begin(), out.tiles.end(), tileCoordLess);
  return out;
}

std::vector<TileCoord> canvasTiles(const Document& doc) {
  std::vector<TileCoord> tiles;
  const int32_t nx = tilesAcross(doc.width);
  const int32_t ny = tilesAcross(doc.height);
  tiles.reserve(static_cast<size_t>(nx) * static_cast<size_t>(ny));
  for (int32_t y = 0; y < ny; ++y)
    for (int32_t x = 0; x < nx; ++x) tiles.push_back(TileCoord{x, y});
  return tiles;
}

size_t canvasTileCount(const Document& doc) noexcept {
  return static_cast<size_t>(tilesAcross(doc.width)) * static_cast<size_t>(tilesAcross(doc.height));
}

bool preferFullRecomposite(size_t dirtyTiles, size_t canvasTiles) noexcept {
  if (canvasTiles == 0) return true;
  return static_cast<double>(dirtyTiles) >=
         kFullRecompositeTileFraction * static_cast<double>(canvasTiles);
}

}  // namespace np
