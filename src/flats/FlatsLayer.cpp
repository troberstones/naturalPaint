#include "flats/FlatsLayer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "color/Space.hpp"
#include "core/Composite.hpp"
#include "core/TextContent.hpp"
#include "core/Tile.hpp"
#include "core/VectorShape.hpp"

namespace np {

namespace {

struct Entry {
  uint64_t hash = 0;
  std::shared_ptr<const FlatEvaluation> eval;
  std::shared_ptr<const TileStore> tiles;
};

// One entry per layer id, never a history of them -- core/VectorRaster's
// rule, for its reason: a layer only ever needs its current evaluation.
std::unordered_map<uint64_t, Entry>& cache() {
  static std::unordered_map<uint64_t, Entry> c;
  return c;
}

void mix(uint64_t& h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
}

uint64_t layerSignature(const Layer& l, const GradientTable& gradients) {
  uint64_t h = 1469598103934665603ull;
  mix(h, l.id);
  mix(h, static_cast<uint64_t>(l.kind));
  mix(h, l.visible);
  uint32_t ob = 0;
  std::memcpy(&ob, &l.opacity, sizeof ob);
  mix(h, ob);
  for (const char ch : l.blend) mix(h, static_cast<uint8_t>(ch));
  mix(h, l.clipped);
  mix(h, l.mask.has_value());
  if (l.rgbTiles) {
    for (const auto& [coord, tile] : *l.rgbTiles) {
      mix(h, static_cast<uint64_t>(static_cast<uint32_t>(coord.x)) << 32 | static_cast<uint32_t>(coord.y));
      mix(h, reinterpret_cast<uintptr_t>(&tile));
    }
  }
  if (l.pigmentTiles) {
    for (const auto& [coord, tile] : *l.pigmentTiles) {
      mix(h, static_cast<uint64_t>(static_cast<uint32_t>(coord.x)) << 32 | static_cast<uint32_t>(coord.y));
      mix(h, reinterpret_cast<uintptr_t>(&tile));
    }
  }
  if (l.mask) {
    for (const auto& [coord, tile] : *l.mask) {
      mix(h, static_cast<uint64_t>(static_cast<uint32_t>(coord.x)) << 32 | static_cast<uint32_t>(coord.y));
      mix(h, reinterpret_cast<uintptr_t>(&tile));
    }
  }
  // Parametric kinds beneath: their content hash is their signature.
  if (l.kind == LayerKind::Vector) mix(h, vectorContentHash(l.shapes, gradients));
  if (l.kind == LayerKind::Text) mix(h, textContentHash(l.text));
  if (l.kind == LayerKind::Flats) mix(h, flatsContentHash(l.flats));
  if (l.kind == LayerKind::Adjustment || l.kind == LayerKind::Group) mix(h, l.ops.size());
  return h;
}

std::shared_ptr<const FlatEvaluation> evaluate(const Document& doc, const std::vector<size_t>& source,
                                               const FlatsContent& content, uint64_t layerId, uint64_t hash) {
  auto& c = cache();
  auto it = c.find(layerId);
  if (it != c.end() && it->second.hash == hash && it->second.eval) return it->second.eval;
  const std::vector<uint8_t> rgba = flatsSourceRgba8(doc, source);
  auto eval = std::make_shared<const FlatEvaluation>(
      flatEvaluate(rgba.data(), doc.width, doc.height, content));
  Entry e;
  e.hash = hash;
  e.eval = eval;
  c[layerId] = std::move(e);
  return eval;
}

}  // namespace

bool flatsLayerEvaluable(const Document& doc, size_t index) noexcept {
  return index < doc.layers.size() && doc.layers[index].kind == LayerKind::Flats && doc.width > 0 &&
         doc.height > 0;
}

std::vector<size_t> flatsSourceLayers(const Document& doc, size_t index) {
  const size_t below = std::min(index, doc.layers.size());
  std::vector<size_t> marked;
  for (size_t i = 0; i < below; i++)
    if (doc.layers[i].flatsReference) marked.push_back(i);
  // A reference marked ABOVE the Flats layer is not a source: it would make
  // the layer read its own result's neighbourhood, and the rule the user
  // reasons about ("flats sit over the drawing") would stop holding.
  if (!marked.empty()) return marked;
  std::vector<size_t> all(below);
  for (size_t i = 0; i < below; i++) all[i] = i;
  return all;
}

const FlatsBakeSourceRow kFlatsBakeSources[kFlatsBakeSourceCount] = {
    {FlatsBakeSource::ExcludeTarget, "Below fill",
     "Segment everything except the layer being filled. The default: it stops the second fill "
     "from seeing the first fill's pixels as line art."},
    {FlatsBakeSource::AllLayers, "All layers",
     "Segment the whole composite, the layer being filled included -- Photoshop's \"sample all "
     "layers\". Use it when the line art is on the layer you are filling."},
    {FlatsBakeSource::ReferenceLayer, "Reference",
     "Segment only the layers marked as the flatting reference. Falls back to All layers when "
     "nothing is marked."},
};

std::vector<size_t> flatsBakeSourceLayers(const Document& doc, size_t targetIndex,
                                          FlatsBakeSource mode) {
  std::vector<size_t> out;
  if (mode == FlatsBakeSource::ReferenceLayer) {
    for (size_t i = 0; i < doc.layers.size(); i++)
      if (doc.layers[i].flatsReference) out.push_back(i);
    // Nothing marked is not an error and must not segment an empty page --
    // it falls through to the whole composite, which is what the mode meant
    // before a reference existed.
    if (!out.empty()) return out;
    mode = FlatsBakeSource::AllLayers;
  }
  for (size_t i = 0; i < doc.layers.size(); i++) {
    if (mode == FlatsBakeSource::ExcludeTarget && i == targetIndex) continue;
    out.push_back(i);
  }
  return out;
}

uint64_t flatsSourceSignature(const Document& doc, const std::vector<size_t>& layers) {
  uint64_t h = 0x243f6a8885a308d3ull;
  mix(h, static_cast<uint64_t>(doc.width) << 32 | static_cast<uint32_t>(doc.height));
  for (size_t i : layers) {
    if (i >= doc.layers.size()) continue;
    const Layer& l = doc.layers[i];
    // An invisible source contributes its identity and nothing else, so
    // hiding or showing it is a change while editing it is not.
    if (!l.visible) {
      mix(h, l.id);
      continue;
    }
    mix(h, layerSignature(l, doc.gradients));
  }
  return h;
}

std::vector<uint8_t> flatsSourceRgba8(const Document& doc, const std::vector<size_t>& layers) {
  const size_t n = static_cast<size_t>(doc.width) * doc.height;
  std::vector<uint8_t> out(n * 4, 0);
  if (n == 0) return out;
  // A shallow copy: `TileStore` shares tiles, so this costs the slot maps
  // and nothing else. Only the source layers survive; the compositor then
  // walks them exactly as it would for the document itself.
  Document below = doc;
  below.layers.clear();
  below.layers.reserve(layers.size());
  // The source indices actually kept, so the loop below can ask "what WAS
  // directly beneath this layer" -- `layers` itself cannot be indexed in step
  // with `below.layers` once an out-of-range entry has been skipped.
  std::vector<size_t> kept;
  kept.reserve(layers.size());
  for (size_t i : layers) {
    if (i >= doc.layers.size()) continue;
    below.layers.push_back(doc.layers[i]);
    kept.push_back(i);
  }
  // ---- the two relationships that reach ACROSS layers --------------------
  //
  // A subset can break a clip (`clipped` reads whatever is directly below,
  // PRD C9) and a group membership (`parent` names a Group layer's
  // `groupTag`). Both are dropped where the thing they point at is not in the
  // subset, because a clip whose base is gone would clip to nothing and a
  // member whose Group is gone would look for a parent that is not there --
  // and neither relationship is part of "what does this layer draw", which is
  // the only question a segmentation asks.
  //
  // **Dropped where BROKEN, not dropped wholesale.** An earlier version of
  // this cleared both flags unconditionally, with a comment claiming the
  // default source could not be affected because it is a contiguous prefix.
  // That was wrong: a clipped layer inside the prefix still has its base
  // inside the prefix, so clearing the flag silently stopped it clipping and
  // changed the segmentation of every document that used one. Nothing in the
  // suite covered it. The condition below is what keeps the default source
  // byte-for-byte identical, so it is asserted rather than argued
  // (`app/selftest/FlatsSource.cpp`).
  for (size_t k = 0; k < below.layers.size(); k++) {
    Layer& l = below.layers[k];
    // The clip survives only if the layer that WAS directly beneath it is
    // still directly beneath it -- ids, because indices have moved.
    if (l.clipped) {
      const size_t src = kept[k];
      const bool baseKept = src > 0 && k > 0 && below.layers[k - 1].id == doc.layers[src - 1].id;
      if (!baseKept) l.clipped = false;
    }
    if (!l.parent.empty()) {
      bool groupKept = false;
      for (const Layer& g : below.layers)
        if (g.groupTag == l.parent) groupKept = true;
      if (!groupKept) l.parent.clear();
    }
  }
  const std::vector<float> pre = compositeDocumentPremultiplied(below);
  if (pre.size() != n * 4) return out;
  for (size_t i = 0; i < n; i++) {
    const float a = pre[i * 4 + 3];
    if (!(a > 0.f)) continue;
    const float inv = 1.f / a;
    for (int ch = 0; ch < 3; ch++) {
      const float enc = srgbEncode(pre[i * 4 + ch] * inv);
      out[i * 4 + ch] = static_cast<uint8_t>(std::lround(std::min(1.f, std::max(0.f, enc)) * 255.f));
    }
    out[i * 4 + 3] = static_cast<uint8_t>(std::lround(std::min(1.f, a) * 255.f));
  }
  return out;
}

std::shared_ptr<const FlatEvaluation> flatsEvaluateLayer(const Document& doc, size_t index) {
  if (!flatsLayerEvaluable(doc, index)) return nullptr;
  const Layer& layer = doc.layers[index];
  const std::vector<size_t> source = flatsSourceLayers(doc, index);
  uint64_t hash = flatsContentHash(layer.flats);
  mix(hash, flatsSourceSignature(doc, source));
  return evaluate(doc, source, layer.flats, layer.id, hash);
}

std::shared_ptr<const FlatEvaluation> flatsPeekEvaluation(const Document& doc, size_t index) {
  // The first three lines of `evaluate()` and nothing else -- the same key,
  // the same staleness test, and a null instead of the recompute.
  if (!flatsLayerEvaluable(doc, index)) return nullptr;
  const Layer& layer = doc.layers[index];
  uint64_t hash = flatsContentHash(layer.flats);
  mix(hash, flatsSourceSignature(doc, flatsSourceLayers(doc, index)));
  auto& c = cache();
  auto it = c.find(layer.id);
  if (it == c.end() || it->second.hash != hash || !it->second.eval) return nullptr;
  return it->second.eval;
}

std::shared_ptr<const FlatEvaluation> flatsEvaluateSource(const Document& doc, uint64_t cacheKey,
                                                          const std::vector<size_t>& layers,
                                                          const FlatsContent& content) {
  if (doc.width <= 0 || doc.height <= 0) return nullptr;
  uint64_t hash = flatsContentHash(content);
  mix(hash, flatsSourceSignature(doc, layers));
  mix(hash, 0x5bd1e995u);  // never collides with a Flats layer's own evaluation
  return evaluate(doc, layers, content, cacheKey, hash);
}

TileStore flatsRasterize(const FlatEvaluation& e) {
  TileStore out;
  if (e.labels.empty()) return out;
  std::vector<uint8_t> rgba(static_cast<size_t>(e.w) * e.h * 4);
  flatRenderRgba8(e, rgba.data());
  // The 8-bit display palette decodes ONCE per distinct colour: a flat has a
  // few hundred colours and a few million pixels.
  std::unordered_map<uint32_t, std::array<float, 3>> decoded;
  for (int ty = 0; ty < e.h; ty += kTileSize) {
    for (int tx = 0; tx < e.w; tx += kTileSize) {
      // Tiles allocate only where content exists (PRD C2): scan first.
      bool any = false;
      for (int y = ty; y < std::min(e.h, ty + kTileSize) && !any; y++)
        for (int x = tx; x < std::min(e.w, tx + kTileSize); x++)
          if (rgba[(static_cast<size_t>(y) * e.w + x) * 4 + 3]) { any = true; break; }
      if (!any) continue;
      Tile& tile = out.getOrCreate(tileCoordAt(PixelCoord{tx, ty}));
      for (int y = ty; y < std::min(e.h, ty + kTileSize); y++) {
        for (int x = tx; x < std::min(e.w, tx + kTileSize); x++) {
          const size_t i = (static_cast<size_t>(y) * e.w + x) * 4;
          if (!rgba[i + 3]) continue;
          const uint32_t key = static_cast<uint32_t>(rgba[i]) << 16 | static_cast<uint32_t>(rgba[i + 1]) << 8 | rgba[i + 2];
          auto it = decoded.find(key);
          if (it == decoded.end())
            it = decoded.emplace(key, std::array<float, 3>{srgbDecode(rgba[i] / 255.f), srgbDecode(rgba[i + 1] / 255.f),
                                                           srgbDecode(rgba[i + 2] / 255.f)}).first;
          const float a = rgba[i + 3] / 255.f;
          // Premultiplied, as every Tile is.
          tile.writePixel(tileLocalOffset(PixelCoord{x, y}),
                          {it->second[0] * a, it->second[1] * a, it->second[2] * a, a});
        }
      }
    }
  }
  return out;
}

std::shared_ptr<const TileStore> flatsLayerTiles(const Document& doc, size_t index) {
  std::shared_ptr<const FlatEvaluation> eval = flatsEvaluateLayer(doc, index);
  if (!eval) return nullptr;
  auto& c = cache();
  Entry& e = c[doc.layers[index].id];
  if (!e.tiles || e.eval != eval) {
    e.eval = eval;
    e.tiles = std::make_shared<const TileStore>(flatsRasterize(*eval));
  }
  return e.tiles;
}

void flatsForgetLayersNotIn(const Document& doc) {
  auto& c = cache();
  for (auto it = c.begin(); it != c.end();) {
    bool present = false;
    for (const Layer& l : doc.layers)
      if (l.id == it->first) { present = true; break; }
    it = present ? std::next(it) : c.erase(it);
  }
}

void flatsForgetAll() { cache().clear(); }

size_t flatsCacheEntryCount() noexcept { return cache().size(); }

Selection flatsFillSelection(const FlatEvaluation& e, int fillId) {
  Selection sel;
  if (!fillId || e.labels.empty()) return sel;
  const std::vector<int32_t> lut = e.rootLut();
  for (int y = 0; y < e.h; y++) {
    for (int x = 0; x < e.w; x++) {
      if (lut[e.labels[static_cast<size_t>(y) * e.w + x]] != fillId) continue;
      sel.tiles.getOrCreate(tileCoordAt(PixelCoord{x, y})).writeCoverage(tileLocalOffset(PixelCoord{x, y}), 1.0f);
    }
  }
  return sel;
}

}  // namespace np
