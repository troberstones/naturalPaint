#include "core/DirtyTiles.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <type_traits>
#include <unordered_set>

#include "core/Blend.hpp"
#include "core/Composite.hpp"
#include "core/Mask.hpp"
#include "core/Pigment.hpp"
#include "core/TextContent.hpp"
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

// Every tile coordinate `layer` occupies, RGB or Pigment, added to `dirty`.
// This is the whole of "a layer's own tile footprint": core/Composite.cpp's
// walk visits a non-Adjustment, non-mixed layer's own store and nothing wider
// (`for (coord, tile : *layer.rgbTiles) if (visits(coord)) ...`), so a change
// that only moves texels the layer itself can produce -- visible, opacity,
// blend -- cannot move a texel outside this set (core/DirtyTiles.hpp §4: no
// op reads a neighbour).
void addLayerOwnTiles(const Layer& layer, std::unordered_set<TileCoord>& dirty) {
  if (layer.rgbTiles)
    for (const auto& [coord, tile] : *layer.rgbTiles) {
      (void)tile;
      dirty.insert(coord);
    }
  if (layer.pigmentTiles)
    for (const auto& [coord, tile] : *layer.pigmentTiles) {
      (void)tile;
      dirty.insert(coord);
    }
}

// The base layer index `clipRuns()` found for `layerIndex`, or `nullopt` when
// that layer is not clipped to anything in `runs` (unclipped, or clipped
// without a base -- core/Composite.hpp §12). Linear in the layer count, which
// is fine here: called at most a few times per call to `documentDirtyTiles()`,
// never per tile.
std::optional<size_t> clipBaseOf(const ClipRuns& runs, size_t layerIndex) {
  for (size_t base = 0; base < runs.members.size(); ++base)
    if (std::find(runs.members[base].begin(), runs.members[base].end(), layerIndex) !=
        runs.members[base].end())
      return base;
  return std::nullopt;
}

// Whether flipping the `clipped` flag of a layer in `changed` moved the
// clip-BASE ASSIGNMENT of some OTHER layer too. `clipRuns()`'s own base
// variable only advances at a non-clipped layer, so un-clipping the middle of
// a run of clipped layers re-parents every layer above it in that run to a
// new base (the layer just un-clipped, or further "clipped without a base"
// if that layer does not hold pixels) -- a layer that itself never appears in
// `before`/`after`'s per-property diff at all, and whose own composited
// pixels, over its own tile footprint, can still have moved. That footprint
// is not tracked anywhere in `changed`'s own per-layer tile sets, so the only
// safe answer once this is detected is the whole canvas. Layers in `changed`
// are excluded from the scan because their OWN base reassignment is exactly
// what the per-layer union in pass 4 already accounts for -- this function
// asks only about the layers that did not change.
bool clipTopologyRippled(const ClipRuns& before, const ClipRuns& after,
                         const std::vector<size_t>& changed, size_t layerCount) {
  for (size_t j = 0; j < layerCount; ++j) {
    if (std::find(changed.begin(), changed.end(), j) != changed.end()) continue;
    if (before.clippedToBase[j] != after.clippedToBase[j] ||
        before.clippedWithoutBase[j] != after.clippedWithoutBase[j])
      return true;
    if (before.clippedToBase[j] && clipBaseOf(before, j) != clipBaseOf(after, j)) return true;
  }
  return false;
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
    case FullRecompositeReason::VectorGeometryChanged: return "vector layer geometry changed";
    case FullRecompositeReason::TextContentChanged: return "text layer content changed";
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
    case FullRecompositeReason::VectorGeometryChanged:
    case FullRecompositeReason::TextContentChanged:
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

  const size_t n = after.layers.size();

  // --- Pass 1: the reasons that are never narrowed -------------------------
  //
  // Checked for every layer before anything else, and *not* interleaved with
  // pass 2 below, so that a layer carrying both an ops change and (say) an
  // opacity change in the same edit is caught here rather than mistaken for a
  // pure opacity change that pass 2 would then narrow incorrectly.
  for (size_t i = 0; i < n; ++i) {
    const Layer& a = before.layers[i];
    const Layer& b = after.layers[i];
    if (a.kind != b.kind) return whole(FullRecompositeReason::LayerKindChanged, i);
    if (!opStacksEqual(a.ops, b.ops)) return whole(FullRecompositeReason::LayerOpsChanged, i);
    if (a.mask.has_value() != b.mask.has_value())
      return whole(FullRecompositeReason::LayerMaskPresenceChanged, i);
    if (a.rgbTiles.has_value() != b.rgbTiles.has_value() ||
        a.pigmentTiles.has_value() != b.pigmentTiles.has_value())
      return whole(FullRecompositeReason::LayerStoragePresenceChanged, i);
    // A Vector layer's content is `shapes`, which no comparison above reaches.
    // Hashed rather than compared field by field so that adding a field to
    // `VectorShape` cannot silently fall out of this test -- the same argument
    // core/VectorShape.hpp makes for hashing rather than counting revisions.
    //
    // **Deliberately coarse: a geometry edit forces a FULL recomposite.** The
    // narrow answer is available -- the union of the old and new shape bounds,
    // via `vectorShapesBounds()` -- but it has to be threaded through both the
    // pass-2 fast path and the pass-3 per-layer path, and getting it wrong
    // reintroduces exactly the invisible-edit bug this block exists to close.
    // Correct first; narrowing is a measured optimisation with its own proof,
    // and Stage 4's manipulator drag is where it will be worth taking.
    if (a.kind == LayerKind::Vector && b.kind == LayerKind::Vector &&
        vectorContentHash(a.shapes) != vectorContentHash(b.shapes))
      return whole(FullRecompositeReason::VectorGeometryChanged, i);
    // A Text layer's content is `text`, which no comparison above reaches
    // either -- and it is worse than the Vector case, because a Text layer
    // holds no `shapes` to fall back on. Every word of the paragraph above
    // applies: hashed rather than compared field by field, and deliberately
    // coarse.
    //
    // **`textContentHash()` and not `vectorContentHash(textContentToShapes())`.**
    // The second would be correct and would SHAPE THE TEXT TWICE ON EVERY
    // EDIT -- once here and once in core/VectorRaster -- on the hot path of a
    // user holding a key down. The hash is over the content that produces the
    // geometry, which is the same question asked one step earlier and for
    // free.
    if (a.kind == LayerKind::Text && b.kind == LayerKind::Text &&
        textContentHash(a.text) != textContentHash(b.text))
      return whole(FullRecompositeReason::TextContentChanged, i);
  }

  // --- Pass 2: which layers changed visible/opacity/blend/clipped ----------
  //
  // Gathered rather than acted on immediately, because narrowing one of these
  // safely requires knowing about every OTHER layer that also changed one of
  // them in the same edit -- see the safety pass below.
  std::vector<size_t> propertyChanged;
  for (size_t i = 0; i < n; ++i) {
    const Layer& a = before.layers[i];
    const Layer& b = after.layers[i];
    // Bitwise, for the reason `sameBits()` gives: a NaN opacity is clamped to
    // 0 by `layerCoverage()` and must not be reported as changing every frame.
    if (a.visible != b.visible || !sameBits(a.opacity, b.opacity) || a.blend != b.blend ||
        a.clipped != b.clipped)
      propertyChanged.push_back(i);
  }

  if (propertyChanged.empty()) {
    // Nothing but tile content differs anywhere -- the original narrow path.
    std::unordered_set<TileCoord> dirty;
    for (size_t i = 0; i < n; ++i) {
      const Layer& a = before.layers[i];
      const Layer& b = after.layers[i];
      diffStore(a.rgbTiles ? &*a.rgbTiles : nullptr, b.rgbTiles ? &*b.rgbTiles : nullptr, dirty);
      diffStore(a.pigmentTiles ? &*a.pigmentTiles : nullptr,
                b.pigmentTiles ? &*b.pigmentTiles : nullptr, dirty);
      diffStore(a.mask ? &*a.mask : nullptr, b.mask ? &*b.mask : nullptr, dirty);
    }
    out.tiles.assign(dirty.begin(), dirty.end());
    std::sort(out.tiles.begin(), out.tiles.end(), tileCoordLess);
    return out;
  }

  // --- Pass 3a: clip TOPOLOGY ripple, before anything per-layer ------------
  //
  // `clipRuns()`'s own "base" is a running variable that advances only at a
  // non-clipped layer (core/Composite.hpp §12), so un-clipping the MIDDLE of
  // a run of clipped layers re-parents every layer ABOVE it in that run to a
  // new base -- a layer that never appears in `propertyChanged` at all (it
  // did not itself change), and whose own composited pixels, over its own
  // tile footprint, still moved. Computed once, only when some changed layer
  // actually flipped `clipped` (a document with no clip change pays nothing
  // here, the same "free when unused" shape core/Composite.hpp §17 gives
  // `clipRuns()` itself), and reused below rather than recomputed.
  const bool anyClipChanged = std::any_of(propertyChanged.begin(), propertyChanged.end(),
                                          [&](size_t i) {
                                            return before.layers[i].clipped !=
                                                   after.layers[i].clipped;
                                          });
  std::optional<ClipRuns> clipsBefore, clipsAfter;
  if (anyClipChanged) {
    clipsBefore = clipRuns(before);
    clipsAfter = clipRuns(after);
    if (clipTopologyRippled(*clipsBefore, *clipsAfter, propertyChanged, n)) {
      const size_t i = *std::find_if(propertyChanged.begin(), propertyChanged.end(),
                                     [&](size_t j) {
                                       return before.layers[j].clipped != after.layers[j].clipped;
                                     });
      return whole(FullRecompositeReason::LayerClipChanged, i);
    }
  }

  // --- Pass 3b: the safety check every narrowed layer must pass ------------
  //
  // core/DirtyTiles.hpp §4: no blend op reads a neighbour, so a layer's own
  // visible/opacity/blend can only move texels inside that layer's own tile
  // footprint (core/Composite.cpp's walk visits exactly that set for a
  // non-Adjustment, non-mixed layer) -- and, for a clipped flag whose
  // topology did NOT ripple (pass 3a already returned otherwise), that
  // footprint union its clip base's (§17: "the base's tiles are the clipping
  // run's whole extent"). Three things break that claim, checked here for
  // every changed layer, and any one of them forces the WHOLE document back
  // to `whole()` (never just that layer) because a mix-pairing change can
  // ripple to a layer that did not itself change (see the comment below):
  //
  //   * an Adjustment layer -- its op stack "reaches everything below it"
  //     (§3), which is not this layer's own footprint at all;
  //   * a layer with no pixel storage of its own -- nothing to narrow to;
  //   * entanglement with a Pigment `Mix` pairing. `mixPairing()` depends on
  //     `blend` (a pair needs `blend == "mix"`) and on `clipped`
  //     (`blendModeAvailableForLayer()` refuses either half of a pair being
  //     clipped), and core/Composite.cpp's mixed-pair branch composites over
  //     the UNION of the upper and lower layer's tiles, not either layer's own
  //     footprint -- a texel where only the *other* half of the pair has a
  //     tile still depends on both layers' coverage. Narrowing that correctly
  //     would mean tracking the pairing's neighbour too, and because the
  //     pairing is greedy and bottom-up, a change at layer `i` can also flip
  //     whether layer `i+1` pairs with it, which is why any entanglement here
  //     is answered with a full recomposite rather than a computed union.
  for (const size_t i : propertyChanged) {
    const Layer& a = before.layers[i];
    const Layer& b = after.layers[i];
    const bool visD = a.visible != b.visible;
    const bool opD = !sameBits(a.opacity, b.opacity);
    const bool blD = a.blend != b.blend;
    const FullRecompositeReason r = visD  ? FullRecompositeReason::LayerVisibilityChanged
                                    : opD  ? FullRecompositeReason::LayerOpacityChanged
                                    : blD  ? FullRecompositeReason::LayerBlendChanged
                                           : FullRecompositeReason::LayerClipChanged;

    if (a.kind == LayerKind::Adjustment) return whole(r, i);
    // `a` and `b` agree on kind and on rgb/pigmentTiles presence (pass 1
    // already forced `whole()` otherwise), so `layerHoldsPixels(a) ==
    // layerHoldsPixels(b)` and either may be asked.
    if (!layerHoldsPixels(b)) return whole(r, i);

    const MixPairing pairingBefore = mixPairing(before);
    const MixPairing pairingAfter = mixPairing(after);
    if (pairingBefore.mixedWithBelow[i] || pairingBefore.consumedByAbove[i] ||
        pairingAfter.mixedWithBelow[i] || pairingAfter.consumedByAbove[i])
      return whole(r, i);
  }

  // --- Pass 4: every changed layer is safe -- build the narrow set ---------
  //
  // The ordinary tile-content diff, over every layer regardless of whether it
  // is one of the changed ones -- an unrelated layer may carry an ordinary
  // paint edit in the same `before`/`after` pair, and this is what catches it
  // -- unioned with each changed layer's own tile footprint (which the
  // content diff alone would miss entirely for a pure property change: the
  // store itself never moved) and, for a `clipped` flip, its clip base's
  // footprint too, from WHICHEVER of before/after the base relationship holds
  // in (the flag is flipping, so at most one side has one).
  std::unordered_set<TileCoord> dirty;
  for (size_t i = 0; i < n; ++i) {
    const Layer& a = before.layers[i];
    const Layer& b = after.layers[i];
    diffStore(a.rgbTiles ? &*a.rgbTiles : nullptr, b.rgbTiles ? &*b.rgbTiles : nullptr, dirty);
    diffStore(a.pigmentTiles ? &*a.pigmentTiles : nullptr,
              b.pigmentTiles ? &*b.pigmentTiles : nullptr, dirty);
    diffStore(a.mask ? &*a.mask : nullptr, b.mask ? &*b.mask : nullptr, dirty);
  }

  // `clipsBefore`/`clipsAfter` were already computed above whenever
  // `anyClipChanged` -- reused rather than recomputed, and pass 3a has
  // already ruled out a topology ripple, so a plain per-layer base lookup is
  // safe here.
  for (const size_t i : propertyChanged) {
    addLayerOwnTiles(before.layers[i], dirty);
    addLayerOwnTiles(after.layers[i], dirty);
    if (before.layers[i].clipped != after.layers[i].clipped) {
      if (const std::optional<size_t> base = clipBaseOf(*clipsBefore, i))
        addLayerOwnTiles(before.layers[*base], dirty);
      if (const std::optional<size_t> base = clipBaseOf(*clipsAfter, i))
        addLayerOwnTiles(after.layers[*base], dirty);
    }
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
