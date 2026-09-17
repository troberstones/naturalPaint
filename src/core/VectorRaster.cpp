#include "core/VectorRaster.hpp"

#include "brush/StrokesLayer.hpp"
#include "flats/FlatsLayer.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/PathFlatten.hpp"
#include "core/PathStroke.hpp"
#include "core/Parallel.hpp"
#include "core/SelectionMask.hpp"
#include "core/TextContent.hpp"

namespace np {
namespace {

// The flattening tolerance used when rasterising a layer into document texels.
//
// A tenth of a texel: below what the 1/255 coverage quantisation downstream
// can express, so tightening it further buys nothing visible while costing
// segments quadratically (core/PathFlatten's count goes as 1/sqrt(tol)).
// Deliberately NOT the tolerance an on-screen overlay should use -- an overlay
// at 8x zoom needs an eighth of this, and that is the caller's decision.
constexpr float kDocumentTolerancePx = 0.1f;

// What a shape is painted with, resolved once per fill or stroke: either a
// constant straight RGBA, or a gradient to sample per texel.
//
// **`Paint` is not passed down here, deliberately.** Resolving the index into
// the table is the one step that can fail (an index past the end), and it
// happens ONCE at the top of `resolvePaint()` rather than per texel -- so the
// painting loop below has no table, no index and no way to reach past the end
// of one.
struct ResolvedPaint {
  bool paints = false;
  std::array<float, 4> straightRgba{0.0f, 0.0f, 0.0f, 1.0f};
  // Null for a solid paint. Non-null means sample per texel instead.
  const GradientDef* gradient = nullptr;
};

// `paint` against `gradients`, or `paints == false` for anything that draws
// nothing at all.
//
// The three ways to draw nothing, stated together because they are easy to
// conflate and only the first is the common one:
//
//   * `on == false` -- no fill AT ALL, distinct from alpha 0 (core/VectorShape).
//   * a gradient index past the end of the table -- NOTHING, never `rgba` and
//     never the last entry, per `Paint::gradient`'s own contract. A document
//     that opens and renders a colour nobody authored is the failure mode this
//     project refuses; an out-of-range index is a producer's bug and has to
//     look like one.
//   * a gradient with no colour stops -- NOTHING, matching ops/Gradient's
//     `renderGradient()`, which returns 0 for the same input. The other empty
//     case, no OPACITY stops, means fully opaque and is handled inside
//     `gradientOpacityAt()`.
ResolvedPaint resolvePaint(const Paint& paint, const GradientTable& gradients) {
  ResolvedPaint out;
  if (!paint.on) return out;
  if (paint.kind == PaintKind::Gradient) {
    if (paint.gradient >= gradients.size()) return out;
    const GradientDef& def = gradients[paint.gradient];
    if (def.stops.colorStops.empty()) return out;
    out.gradient = &def;
    out.paints = true;
    return out;
  }
  out.straightRgba = paint.rgba;
  out.paints = paint.rgba[3] > 0.0f;
  return out;
}

// Paint one already-built coverage source over the tile store, "over" in
// linear premultiplied space.
//
// `clip` may be null. When present its coverage multiplies the shape's, which
// is exactly what SVG's `clip-path` means and is why core/PathRaster emits
// spans rather than owning a destination: intersecting two coverages is a
// multiply over a row with no intermediate image.
//
// **core/PathRaster is untouched by gradients, and that is the design and not
// an omission.** It emits coverage and never owns a destination or a colour
// (its own section 1), so a gradient is a property of what the caller does
// with a span -- exactly like the clip multiply already sitting in this loop.
// Teaching the rasteriser about paint would give four consumers a colour model
// three of them do not want.
// `clipRect` is the caller's, not recomputed here -- see
// `rasterizeVectorLayer()`'s task-splitting comment for why: a caller that
// wants only one row-band of a shape's own bounding box passes that band
// directly, and `rasterizePath()`/`rasterizeContours()` (core/PathRaster.cpp)
// already support an arbitrary `clip.y0`/`clip.y1` restriction correctly --
// the active-edge list is seeded by scanning every edge with `y0 < clip.y0's
// row bottom` on the very first row, exactly as it would be for a full-height
// clip starting at 0, so shrinking the y-range needs no change there.
void paintCoverage(TileStore& out, const Path& path, const ResolvedPaint& paint,
                   const Selection* clip, const RasterClip& clipRect,
                   PathRasterScratch& scratch) {
  if (!paint.paints) return;
  if (clipRect.x1 <= clipRect.x0 || clipRect.y1 <= clipRect.y0) return;

  const GradientDef* grad = paint.gradient;
  const float r = paint.straightRgba[0], g = paint.straightRgba[1], b = paint.straightRgba[2];
  const float a = paint.straightRgba[3];

  rasterizePath(path, kDocumentTolerancePx, clipRect, scratch,
                [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                  for (int32_t x = x0; x < x1; ++x) {
                    float c = cov[x - x0];
                    const PixelCoord at{x, y};
                    if (clip != nullptr) {
                      c *= selectionCoverageAt(clip, at);
                      if (!(c > 0.0f)) continue;
                    }

                    float sr = r, sg = g, sb = b, sa = a;
                    if (grad != nullptr) {
                      // Texel CENTRES, exactly as ops/Gradient's render loop
                      // samples them -- the same two calls on the same
                      // geometry, so a ramp filling a shape and the same ramp
                      // drawn with the Gradient tool agree texel for texel.
                      const float t = gradientParameterAt(grad->geometry,
                                                          static_cast<float>(x) + 0.5f,
                                                          static_cast<float>(y) + 0.5f);
                      const std::array<float, 4> sample =
                          gradientSampleStraight(grad->stops, t);
                      sr = sample[0];
                      sg = sample[1];
                      sb = sample[2];
                      sa = sample[3];
                    }

                    const float srcA = c * sa;
                    if (!(srcA > 0.0f)) continue;

                    Tile& tile = out.getOrCreate(tileCoordAt(at));
                    const PixelCoord local = tileLocalOffset(at);
                    const std::array<float, 4> dst = tile.readPixel(local);
                    // Source-over, premultiplied. The sample is straight, so
                    // the source premultiplies here and nowhere else --
                    // core/VectorShape.hpp's stated convention.
                    const float inv = 1.0f - srcA;
                    tile.writePixel(local, {sr * srcA + dst[0] * inv, sg * srcA + dst[1] * inv,
                                            sb * srcA + dst[2] * inv, srcA + dst[3] * inv});
                  }
                });
}

// A clip path as an 8-bit sparse coverage mask.
//
// `Selection` is reused rather than a new type invented: it is already
// "sparse, tiled, antialiased coverage in [0,1]", which is precisely a clip,
// and core/SelectionMask.hpp already argues that 8 bits is the right depth for
// exactly this job (Photoshop stores selections at 8 bits regardless of
// document depth). A clip edge therefore quantises to 1/255, which is the same
// step the selection tools have always produced.
Selection clipCoverage(const Path& path, int32_t width, int32_t height,
                       PathRasterScratch& scratch) {
  Selection sel;
  const RasterClip rect = clipForPath(path, width, height);
  if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) return sel;
  rasterizePath(path, kDocumentTolerancePx, rect, scratch,
                [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                  for (int32_t x = x0; x < x1; ++x) {
                    const PixelCoord at{x, y};
                    sel.tiles.getOrCreate(tileCoordAt(at))
                        .writeCoverage(tileLocalOffset(at), cov[x - x0]);
                  }
                });
  return sel;
}

// One tile's worth of `src` composited "over" the same tile of `dst`, in
// place. The per-texel step both `mergeUnitsInto()` below and (before it) a
// single-threaded fold used.
void blendTileOver(Tile& dstTile, const Tile& srcTile) {
  for (int32_t ty = 0; ty < kTileSize; ++ty) {
    for (int32_t tx = 0; tx < kTileSize; ++tx) {
      const PixelCoord local{tx, ty};
      const std::array<float, 4> s = srcTile.readPixel(local);
      if (!(s[3] > 0.0f)) continue;  // nothing to composite at this texel
      const std::array<float, 4> d = dstTile.readPixel(local);
      const float inv = 1.0f - s[3];
      dstTile.writePixel(local, {s[0] + d[0] * inv, s[1] + d[1] * inv, s[2] + d[2] * inv,
                                 s[3] + d[3] * inv});
    }
  }
}

// Folds every unit's private raster into `out`, in `perUnit`'s order, tile by
// tile.
//
// Why this exists rather than every shape painting straight into one shared
// `TileStore`: a fill or a stroke is an independent unit of work -- it reads
// nothing another shape's unit wrote -- so `rasterizeVectorLayer()` below
// rasterises every unit into its OWN, private `TileStore` in parallel
// (core/Parallel.hpp), which sidesteps that header's stated hazard entirely
// rather than working around it: two threads never touch the same
// `TileStore::getOrCreate()` because there is no shared store during the
// parallel step. What parallel units cannot decide among themselves is
// which one wins where two shapes' footprints overlap -- SVG's painter's
// order ("shapes paint bottom to top, fill before stroke") is a property of
// the SEQUENCE, not of any one unit -- so this fold has to happen in that
// original order.
//
// **The fold is itself parallelised, over TILES rather than over units**,
// because a synthetic-but-realistic case (four shapes each spanning most of
// the canvas) measured it at ~20ms against the now-parallel raster step's
// ~15ms -- a fold that undid most of the raster step's own win by staying
// single-threaded. The same core/Parallel.hpp hazard applies (`getOrCreate`
// is not safe to call from two iterations at once), so the same two-phase
// answer: every destination tile the union of `perUnit` touches is reserved
// SERIALLY first (`out.getOrCreate()` once each, before any parallel work
// starts), then the parallel body only walks the *fixed* `Tile*` that
// reservation resolved and reads through a pre-built index -- both
// `TileStore` operations this header documents as safe to call concurrently
// once no writer can touch the map's structure again. Ordering is preserved
// because each destination TILE is owned by exactly one loop iteration,
// which visits every unit that touches it in `perUnit`'s order -- unlike the
// per-unit parallel step above, a tile is never split across two iterations.
//
// **Indexed by tile, not scanned unit by unit**, because `rasterizeVectorLayer()`
// splits a shape into several row-band units (kPaintBandRows), and a naive
// "for each tile, for each unit, find()" fold is O(tiles * units) even
// though most (tile, unit) pairs never overlap -- measured to cost MORE than
// the band-splitting it was folding saved, once the unit count grew past the
// old one-per-shape scheme. `touchedBy` is built by the same single pass
// that finds the tile set, so it costs nothing extra to collect.
void mergeUnitsInto(TileStore& out, const std::vector<TileStore>& perUnit) {
  std::unordered_map<TileCoord, std::vector<uint32_t>> touchedBy;
  for (size_t k = 0; k < perUnit.size(); ++k)
    for (const auto& [coord, tile] : perUnit[k]) {
      (void)tile;
      touchedBy[coord].push_back(static_cast<uint32_t>(k));
    }
  if (touchedBy.empty()) return;

  std::vector<TileCoord> coords;
  coords.reserve(touchedBy.size());
  for (const auto& [coord, list] : touchedBy) {
    (void)list;
    coords.push_back(coord);
  }

  std::vector<Tile*> dstTiles(coords.size());
  for (size_t i = 0; i < coords.size(); ++i) dstTiles[i] = &out.getOrCreate(coords[i]);

  // `const&` so every lookup below goes through the const overloads of
  // `unordered_map::at()` -- concurrent reads with no writer are safe, the
  // same guarantee `TileStore::find()` already relies on in this file.
  const auto& touchedByConst = touchedBy;
  parallelFor(coords.size(), kParallelForDefaultGrain, [&](size_t i) {
    Tile& dstTile = *dstTiles[i];
    for (uint32_t k : touchedByConst.at(coords[i])) {
      const Tile* srcTile = perUnit[k].find(coords[i]);
      if (srcTile != nullptr) blendTileOver(dstTile, *srcTile);
    }
  });
}

// One independent unit of paint work: a shape's fill, or a shape's stroke.
// `rasterizeVectorLayer()` runs every unit in parallel -- see
// `mergeUnitsInto()`'s comment for why that is sound despite two units
// possibly overlapping.
//
// **Not split any finer (e.g. by row-band within a unit), and that was
// measured rather than assumed.** A row-band split was built and profiled
// with `--profile-vector-warp`'s 4-shape/200-anchor fixture: it moved work
// OFF the parallel paint step (a real per-unit win, confirmed by isolating
// it with instrumentation) but added more to the now-serial preparation this
// file used to do inline -- `strokePath()`'s offset-curve computation for
// every unit, previously running concurrently with every OTHER unit's own
// prep, had to move to a serial phase before any bands could be sized. Net
// effect: SLOWER end to end (~35ms) than the un-split, 8-unit version
// (~32ms) it was meant to improve on. Kept at one task per unit for that
// reason; a future attempt should keep `strokePath()` inside the parallel
// step (compute it once per unit, THEN band only the paint call) rather
// than hoisting it out.
struct PaintUnit {
  const VectorShape* shape = nullptr;
  bool isStroke = false;
};

}  // namespace

TileStore rasterizeVectorLayer(const std::vector<VectorShape>& shapes,
                               const GradientTable& gradients, int32_t width, int32_t height) {
  TileStore out;
  if (width <= 0 || height <= 0) return out;

  // SVG's order, flattened into a list of independent units: shapes bottom to
  // top, and within a shape the fill before the stroke -- both properties of
  // this list's ORDER, not of anything a unit reads from another, which is
  // what makes running the list in parallel and then folding it back
  // together in this same order correct (see `mergeUnitsInto()`).
  std::vector<PaintUnit> units;
  units.reserve(shapes.size() * 2);
  for (const VectorShape& shape : shapes) {
    units.push_back(PaintUnit{&shape, false});
    if (shape.stroke.on && shape.strokeStyle.width > 0.0f)
      units.push_back(PaintUnit{&shape, true});
  }
  if (units.empty()) return out;

  // One store and one `PathRasterScratch` PER UNIT rather than shared: both
  // are exactly the per-thread state `parallelFor`'s body must own for
  // itself (core/Parallel.hpp's contract -- "body must tolerate being called
  // from multiple threads concurrently, with... no synchronization supplied
  // by this function"), and a `PathRasterScratch` reused across shapes was
  // never more than an allocation-count optimisation (this file's own
  // comment on the single-scratch version this replaces), never a
  // correctness requirement -- nothing about a scanline rasterisation
  // depends on which buffer backs its accumulators. `strokePath()`'s offset
  // curve, the clip rasterisation and the paint itself all run inside this
  // one parallel step -- see `PaintUnit`'s comment on why pulling any of
  // that out to a serial "prepare" phase measured slower.
  std::vector<TileStore> perUnit(units.size());
  parallelFor(units.size(), kParallelForDefaultGrain, [&](size_t i) {
    const PaintUnit& unit = units[i];
    const VectorShape& shape = *unit.shape;
    PathRasterScratch scratch;
    std::optional<Selection> clip;
    if (shape.clip.has_value()) {
      clip = clipCoverage(*shape.clip, width, height, scratch);
      // An engaged clip with no coverage anywhere hides the shape entirely.
      // Distinct from no clip at all, exactly as core/SelectionMask.hpp
      // distinguishes an empty selection from an absent one -- and getting
      // it backwards here would make a clipped-to-nothing shape paint over
      // everything. Recomputed once per unit (so twice for a shape with both
      // a fill and a stroke) rather than shared across a shape's units,
      // trading a little redundant clip work for units that are fully
      // independent of one another -- the clip path itself is a small
      // fraction of what this file spends per shape.
      if (clip->tiles.occupiedTileCount() == 0) return;
    }
    const Selection* clipPtr = clip.has_value() ? &*clip : nullptr;

    if (!unit.isStroke) {
      const RasterClip bounds = clipForPath(shape.path, width, height);
      paintCoverage(perUnit[i], shape.path, resolvePaint(shape.fill, gradients), clipPtr, bounds,
                    scratch);
      return;
    }
    const Path outline = strokePath(shape.path, shape.strokeStyle, kDocumentTolerancePx);
    if (outline.subpaths.empty()) return;
    const RasterClip bounds = clipForPath(outline, width, height);
    paintCoverage(perUnit[i], outline, resolvePaint(shape.stroke, gradients), clipPtr, bounds,
                  scratch);
  });

  // Fold, in the same order the old single-threaded loop painted: shapes
  // bottom to top, fill before stroke within a shape -- `units` is already
  // built in exactly that order, and `mergeUnitsInto()` is itself parallel.
  mergeUnitsInto(out, perUnit);
  return out;
}

std::shared_ptr<const TileStore> VectorRasterCache::lookup(uint64_t layerId,
                                                           uint64_t hash) const {
  const auto it = byLayer_.find(layerId);
  if (it == byLayer_.end()) return nullptr;
  if (it->second.hash != hash) return nullptr;  // stale: never hand it back
  return it->second.tiles;
}

std::shared_ptr<const TileStore> VectorRasterCache::store(uint64_t layerId, uint64_t hash,
                                                          TileStore tiles) {
  auto shared = std::make_shared<const TileStore>(std::move(tiles));
  byLayer_[layerId] = Entry{hash, shared};
  return shared;
}

void VectorRasterCache::forgetLayersNotIn(const Document& doc) {
  // The Flats evaluation cache is process-wide (flats/FlatsLayer) and is
  // swept here too, so deleting a Flats layer releases its label field on
  // the same call that releases a Vector layer's raster.
  flatsForgetLayersNotIn(doc);
  for (auto it = byLayer_.begin(); it != byLayer_.end();) {
    bool present = false;
    for (const Layer& l : doc.layers)
      if (layerRastersToTiles(l.kind) && l.id == it->first) {
        present = true;
        break;
      }
    it = present ? std::next(it) : byLayer_.erase(it);
  }
}

size_t VectorRasterCache::residentBytes() const noexcept {
  size_t total = 0;
  for (const auto& [id, entry] : byLayer_) {
    (void)id;
    if (entry.tiles) total += entry.tiles->occupiedTileCount() * sizeof(Tile);
  }
  return total;
}

bool layerRastersToTiles(LayerKind kind) noexcept {
  return kind == LayerKind::Vector || kind == LayerKind::Text || kind == LayerKind::Flats ||
         kind == LayerKind::Strokes;
}

bool documentHasVectorLayers(const Document& doc) noexcept {
  for (const Layer& l : doc.layers)
    if (layerRastersToTiles(l.kind)) return true;
  return false;
}

MaterializedDocument::MaterializedDocument(const Document& doc, VectorRasterCache* cache)
    : original_(&doc) {
  // The fast path every existing caller takes: no Vector layer, no copy.
  if (!documentHasVectorLayers(doc)) return;

  Document copy = doc;
  for (size_t index = 0; index < copy.layers.size(); ++index) {
    Layer& layer = copy.layers[index];
    if (!layerRastersToTiles(layer.kind)) continue;

    // **A Flats layer takes its own path** (flats/FlatsLayer, ADR-0009): it
    // evaluates against the composite of the layers BENEATH it in the
    // ORIGINAL document -- `doc`, not `copy`, whose lower layers may already
    // have been rewritten -- and its cache is its own, keyed on its content
    // hash and on a signature of what lies beneath, because a segmentation
    // is seconds of work and `cache` here may be null. The rewrite it lands
    // in is the same one as Text and Vector: an RGB layer sharing the cached
    // tiles.
    if (layer.kind == LayerKind::Flats) {
      std::shared_ptr<const TileStore> flatTiles = flatsLayerTiles(doc, index);
      layer.kind = LayerKind::RGB;
      layer.rgbTiles = flatTiles ? *flatTiles : TileStore{};
      layer.flats = FlatsContent{};
      continue;
    }

    // **A Strokes layer takes the Flats path for the Flats path's reason**
    // (brush/StrokesLayer, PLAN.md phase 8): its dabs may sample the
    // composite BENEATH it, so it must be evaluated against the ORIGINAL
    // document -- `doc`, not `copy`, whose lower layers may already have been
    // rewritten into RGB -- and its cache is its own, keyed on the dab
    // records and on a signature of what lies beneath. Its replay is
    // checkpointed rather than redone, which is the same "cannot be redone on
    // every composite" argument a segmentation makes, one order of magnitude
    // down.
    if (layer.kind == LayerKind::Strokes) {
      std::shared_ptr<const TileStore> dabTiles = strokesLayerTiles(doc, index);
      layer.kind = LayerKind::RGB;
      layer.rgbTiles = dabTiles ? *dabTiles : TileStore{};
      layer.strokes = StrokesContent{};
      continue;
    }

    // **Text takes the same path as Vector because it IS the same path.**
    // `textContentToShapes()` produces the identical `std::vector<VectorShape>`
    // a Vector layer stores, so the only per-kind work is deciding which
    // content to hash and where the shapes come from -- everything past these
    // four lines is shared, which is core/TextContent.hpp section 1's whole
    // claim made concrete.
    const bool isText = layer.kind == LayerKind::Text;
    const uint64_t hash =
        isText ? textContentHash(layer.text) : vectorContentHash(layer.shapes, doc.gradients);
    std::shared_ptr<const TileStore> tiles =
        cache ? cache->lookup(layer.id, hash) : nullptr;
    if (!tiles) {
      // Shaping happens ONLY on a cache miss. A hit skips it entirely, which
      // matters more for text than for geometry: shaping is a CoreText call
      // and a per-frame one would be visible.
      const std::vector<VectorShape> shapes =
          isText ? textContentToShapes(layer.text) : layer.shapes;
      TileStore built = rasterizeVectorLayer(shapes, doc.gradients, doc.width, doc.height);
      tiles = cache ? cache->store(layer.id, hash, std::move(built))
                    : std::make_shared<const TileStore>(std::move(built));
    }

    // Becomes an ordinary RGB layer for the compositor's purposes. Everything
    // else about the layer -- name, opacity, blend, visibility, mask, clip
    // flag, group tag, id -- is carried unchanged, because the compositor
    // reads all of it and a Vector layer must obey the same stack rules as
    // any other.
    //
    // **This document must never be saved.** These layers now claim to be
    // RGB, and writing that out would turn editable geometry into pixels.
    // io/NpaintFile is only ever handed the real document.
    layer.kind = LayerKind::RGB;
    layer.rgbTiles = *tiles;  // shares tiles; copies the slot map only
    layer.shapes.clear();
    layer.text = TextContent{};
  }
  rewritten_ = std::move(copy);
}

}  // namespace np
