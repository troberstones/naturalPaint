#include "app/PathConsumers.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <optional>

#include "brush/RgbDeposit.hpp"
#include "brush/StrokePath.hpp"
#include "core/PathFlatten.hpp"
#include "core/PathRaster.hpp"
#include "core/PathStroke.hpp"
#include "core/TextContent.hpp"
#include "core/VectorRaster.hpp"

namespace np {
namespace {

// How a refusal names the layer it is about.
//
// core/LayerOps' own `layerOpDescribe()` is `layer 2 ("Line pass")` and is the
// better sentence, but it needs a `Document` and an index to produce the
// number. Nothing here is handed a document -- these three functions take the
// one `Layer&` they act on, which is what keeps them callable from --selftest
// with no document at all -- so the name alone is what there is. An unnamed
// layer falls back to its kind rather than to an empty pair of quotes.
std::string describe(const Layer& layer) {
  if (!layer.name.empty()) return "the layer \"" + layer.name + "\"";
  return std::string("the ") + layerKindName(layer.kind) + " layer";
}

// A shape's `clip` as sparse 8-bit coverage.
//
// core/VectorRaster.cpp's `clipCoverage()`, which is in that file's anonymous
// namespace and so cannot be called from here. The duplication is deliberate
// and bounded: it is nine lines, it has no behaviour to drift (a clip is
// coverage, written where the rasteriser emits it), and the alternative --
// exporting it -- would put a `Selection`-returning helper on core/'s public
// surface for two callers in one file each. See PathConsumers.hpp section 2
// for why a clip is the one thing here that is allowed a buffer.
Selection clipCoverageOf(const Path& path, int32_t width, int32_t height,
                         PathRasterScratch& scratch) {
  Selection sel;
  const RasterClip rect = clipForPath(path, width, height);
  if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) return sel;
  rasterizePath(path, kPathConsumerTolerancePx, rect, scratch,
                [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                  for (int32_t x = x0; x < x1; ++x) {
                    const PixelCoord at{x, y};
                    sel.tiles.getOrCreate(tileCoordAt(at))
                        .writeCoverage(tileLocalOffset(at), cov[x - x0]);
                  }
                });
  return sel;
}

// The union of every shape's PATH bounds -- the curve's own tight bounds, with
// no stroke outset.
//
// Not `vectorShapesBounds()`, which adds half the stroke width and the miter
// allowance. That is the right bound for allocating tiles to paint into and
// the wrong one for answering "is this path outside the canvas": a shape whose
// fill is entirely off-canvas but whose stroke outset reaches back onto it
// would be reported as inside, and the caller would get an `ok` result that
// changed nothing.
PathBounds shapePathBounds(const std::vector<VectorShape>& shapes) {
  PathBounds all;
  for (const VectorShape& s : shapes) {
    const PathBounds b = pathTightBounds(s.path);
    if (!b.valid) continue;
    if (!all.valid) {
      all = b;
      continue;
    }
    all.minX = b.minX < all.minX ? b.minX : all.minX;
    all.minY = b.minY < all.minY ? b.minY : all.minY;
    all.maxX = b.maxX > all.maxX ? b.maxX : all.maxX;
    all.maxY = b.maxY > all.maxY ? b.maxY : all.maxY;
  }
  return all;
}

// True when the path's own bounds do not overlap the canvas rectangle at all.
//
// **Not `clipForPath()` returning an empty rectangle**, which is the obvious
// test and is wrong: that function intersects the path's bounds with the
// canvas, so it comes back empty for a path that is outside the canvas AND for
// a path whose bounds have zero WIDTH OR HEIGHT -- a subpath whose anchors are
// all at one point, or a perfectly horizontal one. Using it told a degenerate
// path sitting at (100, 100) of a 256x256 canvas that it "lies entirely
// outside the canvas", which is a sentence a user cannot act on because it is
// not true. Found by sabotage: the assertion that was supposed to cover the
// "enclosed no area" refusal was passing against THIS message instead, because
// both happened to contain the words "no texel".
//
// The two conditions are genuinely different and get genuinely different
// sentences: this one says "move it onto the canvas", the area one says "your
// subpaths enclose nothing".
bool boundsMissCanvas(const PathBounds& b, int32_t width, int32_t height) noexcept {
  if (!b.valid) return true;
  return b.maxX <= 0.0f || b.minX >= static_cast<float>(width) || b.maxY <= 0.0f ||
         b.minY >= static_cast<float>(height);
}

std::string boundsSentence(const PathBounds& b, int32_t width, int32_t height) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "its bounds are x %.2f..%.2f, y %.2f..%.2f, and the canvas is %dx%d.", b.minX,
                b.maxX, b.minY, b.maxY, static_cast<int>(width), static_cast<int>(height));
  return std::string(buf);
}

// True when at least one shape has a path that encloses something
// representable. `pathIsEmpty()` is core/Path.hpp's own test and says nothing
// about whether the enclosed AREA is zero -- a closed triangle with three
// identical anchors is not empty by it, and correctly produces no coverage.
// That case is caught downstream, by the "covered no texel" refusal, which is
// the honest place for it.
bool anyShapeHasGeometry(const std::vector<VectorShape>& shapes) {
  for (const VectorShape& s : shapes)
    if (!pathIsEmpty(s.path)) return true;
  return false;
}

}  // namespace

// ==========================================================================
// Source geometry
// ==========================================================================

PathShapesResult pathConsumerShapes(const Layer& layer) {
  PathShapesResult r;

  if (!layerRastersToTiles(layer.kind)) {
    r.error = std::string("path operation refused: ") + describe(layer) + " is a " +
              layerKindName(layer.kind) +
              " layer, which holds no path geometry. Only a Vector layer (its shapes) and a "
              "Text layer (its text block, shaped into the same shapes) carry paths in this "
              "build -- core/VectorRaster.hpp's `layerRastersToTiles()` is the one predicate "
              "that names both. Draw a path with the Pen tool on a Vector layer first.";
    return r;
  }

  if (layer.kind == LayerKind::Text) {
    std::string shapeError;
    r.shapes = textContentToShapes(layer.text, &shapeError);
    if (!shapeError.empty()) {
      r.error = "path operation refused: " + describe(layer) +
                " could not be shaped into outlines -- " + shapeError;
      return r;
    }
    if (r.shapes.empty()) {
      r.error = "path operation refused: " + describe(layer) +
                " is a Text layer with nothing typed in it yet, so it has no glyph outlines to "
                "use as a path. Type into it with the Text tool first.";
      return r;
    }
  } else {
    r.shapes = layer.shapes;
    if (r.shapes.empty()) {
      r.error = "path operation refused: " + describe(layer) +
                " is a Vector layer with no shapes on it. Draw a path with the Pen tool "
                "first.";
      return r;
    }
  }

  r.ok = true;
  return r;
}

// ==========================================================================
// PRD J2 -- path to selection
// ==========================================================================

PathSelectionResult pathToSelection(const std::vector<VectorShape>& shapes,
                                    const Selection* base, SelectionCombine op, int32_t width,
                                    int32_t height) {
  PathSelectionResult r;

  if (width <= 0 || height <= 0) {
    r.error =
        "path to selection refused: the document has no area (width or height is zero), so "
        "there is nothing for a selection to cover.";
    return r;
  }
  if (!anyShapeHasGeometry(shapes)) {
    r.error =
        "path to selection refused: the path is empty -- no subpath has two anchors, so it "
        "encloses nothing that could be selected. Add at least two anchors to a subpath.";
    return r;
  }

  const PathBounds bounds = shapePathBounds(shapes);
  if (boundsMissCanvas(bounds, width, height)) {
    r.error = "path to selection refused: the path lies entirely outside the canvas, so no "
              "part of it could be selected -- " +
              boundsSentence(bounds, width, height) +
              " Move the path onto the canvas, or use Image > Canvas Size to grow the canvas "
              "around it.";
    return r;
  }

  // The new coverage, before it meets `base`. Written straight into selection
  // tiles from the span callback -- PathConsumers.hpp section 2.
  Selection addend;
  PathRasterScratch scratch;

  for (const VectorShape& shape : shapes) {
    if (pathIsEmpty(shape.path)) continue;
    const RasterClip rect = clipForPath(shape.path, width, height);
    if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) continue;

    std::optional<Selection> clip;
    if (shape.clip.has_value()) {
      clip = clipCoverageOf(*shape.clip, width, height, scratch);
      // An engaged clip covering nothing hides the shape entirely -- distinct
      // from no clip at all, exactly as core/VectorRaster.cpp has it, and
      // getting it backwards would make a clipped-to-nothing shape select the
      // whole of itself.
      if (clip->tiles.occupiedTileCount() == 0) continue;
    }
    const Selection* clipPtr = clip.has_value() ? &*clip : nullptr;

    rasterizePath(shape.path, kPathConsumerTolerancePx, rect, scratch,
                  [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                    for (int32_t x = x0; x < x1; ++x) {
                      const PixelCoord at{x, y};
                      float c = cov[x - x0];
                      if (clipPtr != nullptr) {
                        c *= selectionCoverageAt(clipPtr, at);
                        if (!(c > 0.0f)) continue;
                      }
                      SelectionTile& tile = addend.tiles.getOrCreate(tileCoordAt(at));
                      const PixelCoord local = tileLocalOffset(at);
                      // Shapes union with `max` -- core/SelectionOps.hpp's own
                      // rule, called rather than re-typed, so this union and
                      // the one against `base` below are the same arithmetic.
                      tile.writeCoverage(local, combineCoverage(tile.coverageAt(local), c,
                                                                SelectionCombine::Add));
                    }
                  });
  }

  if (addend.tiles.occupiedTileCount() == 0) {
    r.error =
        "path to selection refused: the path covered no texel of the canvas. Its subpaths "
        "have anchors but enclose no area -- a subpath whose anchors are all at one point, or "
        "a clip that excludes the whole shape, does this.";
    return r;
  }

  // The combine itself. `base` being null is the empty operand, NOT
  // `selectAll()` -- PathConsumers.hpp's note on the parameter.
  const Selection empty;
  r.selection = combineSelections(base != nullptr ? *base : empty, addend, op);

  for (const auto& [coord, tile] : r.selection.tiles) {
    (void)coord;
    for (int32_t ty = 0; ty < kTileSize; ++ty)
      for (int32_t tx = 0; tx < kTileSize; ++tx)
        if (tile.coverageAt(PixelCoord{tx, ty}) > 0.0f) ++r.selectedTexels;
  }

  r.ok = true;
  return r;
}

// ==========================================================================
// PRD J1/J4 -- fill
// ==========================================================================

PathFillResult fillPathIntoLayer(Layer& target, const std::vector<VectorShape>& shapes,
                                 const Selection* selection, int32_t width, int32_t height) {
  PathFillResult r;

  if (width <= 0 || height <= 0) {
    r.error = "fill path refused: the document has no area (width or height is zero).";
    return r;
  }
  if (target.locked) {
    r.error = "fill path refused: " + describe(target) +
              " is locked. A locked layer's content is frozen -- only its visibility and the "
              "lock itself can still be changed (core/LayerOps.hpp). Unlock it first.";
    return r;
  }
  if (target.kind != LayerKind::RGB || !target.rgbTiles.has_value()) {
    r.error = "fill path refused: " + describe(target) + " is a " + layerKindName(target.kind) +
              " layer, which owns no rgba tile store a fill can paint into. A path fill is a "
              "premultiplied source-over of a straight colour, which is what a "
              "LayerKind::RGB layer stores; a Pigment layer holds latent-times-mass and no "
              "alpha channel at all (core/Pigment.hpp), so filling one is a different "
              "arithmetic rather than this one with a flag. Fill an RGB layer instead.";
    return r;
  }
  if (target.alphaLocked) {
    r.error = "fill path refused: " + describe(target) +
              " has its transparent texels locked (Layer > Lock Transparent Pixels). A path "
              "fill puts colour where the path is, including where the layer is wholly "
              "transparent, which is exactly the alpha this flag freezes -- and unlike a "
              "brush stroke there is no alpha-locked composite for a fill in this build "
              "(brush/RgbDeposit.hpp section 4.5 derives one for a dab only). Unlock "
              "transparency, or stroke the path with a brush instead.";
    return r;
  }
  if (!anyShapeHasGeometry(shapes)) {
    r.error =
        "fill path refused: the path is empty -- no subpath has two anchors, so it encloses "
        "nothing to fill. Add at least two anchors to a subpath.";
    return r;
  }

  {
    const PathBounds b = shapePathBounds(shapes);
    if (boundsMissCanvas(b, width, height)) {
      r.error = "fill path refused: the path lies entirely outside the canvas, so the fill "
                "would land nowhere -- " +
                boundsSentence(b, width, height) + " Move the path onto the canvas first.";
      return r;
    }
  }

  TileStore& out = *target.rgbTiles;
  PathRasterScratch scratch;
  size_t changed = 0;

  // core/VectorRaster.cpp's `paintCoverage()`, with the destination changed
  // from a fresh store to the layer's own and the active selection folded into
  // the same multiply the clip already uses. PathConsumers.hpp section 2 says
  // why it is not that function called through `rasterizeVectorLayer()`.
  const auto paint = [&](const Path& path, const std::array<float, 4>& straightRgba,
                         const Selection* clipPtr) {
    const RasterClip rect = clipForPath(path, width, height);
    if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) return;
    const float rr = straightRgba[0], gg = straightRgba[1], bb = straightRgba[2];
    const float aa = straightRgba[3];
    if (!(aa > 0.0f)) return;

    rasterizePath(path, kPathConsumerTolerancePx, rect, scratch,
                  [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                    for (int32_t x = x0; x < x1; ++x) {
                      float c = cov[x - x0];
                      const PixelCoord at{x, y};
                      if (clipPtr != nullptr) {
                        c *= selectionCoverageAt(clipPtr, at);
                        if (!(c > 0.0f)) continue;
                      }
                      // PRD E1. Null means no restriction, which is
                      // core/SelectionMask.hpp's convention and not its
                      // inverse.
                      if (selection != nullptr) {
                        c *= selectionCoverageAt(selection, at);
                        if (!(c > 0.0f)) continue;
                      }
                      const float srcA = c * aa;
                      if (!(srcA > 0.0f)) continue;

                      Tile& tile = out.getOrCreate(tileCoordAt(at));
                      const PixelCoord local = tileLocalOffset(at);
                      const std::array<float, 4> dst = tile.readPixel(local);
                      // Source-over, premultiplied. `straightRgba` is
                      // straight, so the source premultiplies here and nowhere
                      // else -- core/VectorShape.hpp's stated convention.
                      const float inv = 1.0f - srcA;
                      tile.writePixel(local, {rr * srcA + dst[0] * inv, gg * srcA + dst[1] * inv,
                                              bb * srcA + dst[2] * inv, srcA + dst[3] * inv});
                      ++changed;
                    }
                  });
  };

  for (const VectorShape& shape : shapes) {
    if (pathIsEmpty(shape.path)) continue;

    std::optional<Selection> clip;
    if (shape.clip.has_value()) {
      clip = clipCoverageOf(*shape.clip, width, height, scratch);
      if (clip->tiles.occupiedTileCount() == 0) continue;
    }
    const Selection* clipPtr = clip.has_value() ? &*clip : nullptr;

    // SVG's order, and core/VectorRaster.cpp's.
    if (shape.fill.on) paint(shape.path, shape.fill.rgba, clipPtr);
    if (shape.stroke.on && shape.strokeStyle.width > 0.0f) {
      const Path outline = strokePath(shape.path, shape.strokeStyle, kPathConsumerTolerancePx);
      if (!outline.subpaths.empty()) paint(outline, shape.stroke.rgba, clipPtr);
    }
  }

  if (changed == 0) {
    r.error =
        "fill path refused: the fill changed no texel. Either no shape has a fill or a stroke "
        "turned on (an SVG `fill=\"none\"` with no stroke draws nothing at all), the paint's "
        "alpha is zero, the active selection excludes the whole path, or the subpaths enclose "
        "no area.";
    return r;
  }

  r.ok = true;
  r.editLabel = "fill path";
  r.texelsChanged = changed;
  return r;
}

// ==========================================================================
// PRD J3 -- stroke with the current brush
// ==========================================================================

PathStrokeResult strokePathWithBrush(Layer& target, const std::vector<VectorShape>& shapes,
                                     const BrushTip& tip, const Selection* selection,
                                     int32_t width, int32_t height) {
  PathStrokeResult r;

  if (width <= 0 || height <= 0) {
    r.error = "stroke path refused: the document has no area (width or height is zero).";
    return r;
  }
  if (target.locked) {
    r.error = "stroke path refused: " + describe(target) +
              " is locked. A locked layer's content is frozen -- only its visibility and the "
              "lock itself can still be changed (core/LayerOps.hpp). Unlock it first.";
    return r;
  }

  const bool rgbTarget = target.kind == LayerKind::RGB && target.rgbTiles.has_value();
  const bool pigmentTarget =
      target.kind == LayerKind::Pigment && target.pigmentTiles.has_value();
  if (!rgbTarget && !pigmentTarget) {
    r.error = "stroke path refused: " + describe(target) + " is a " +
              layerKindName(target.kind) +
              " layer, which owns no tile store a brush can deposit into. A brush writes "
              "either an RGB layer's rgba tiles (brush/RgbDeposit) or a Pigment layer's "
              "latent-and-mass tiles (brush/Deposit), and this layer has neither. Add a "
              "Pigment or RGB layer and stroke that.";
    return r;
  }
  if (!(tip.radius > 0.0f)) {
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "stroke path refused: the brush radius is %.3f px. Coverage is exactly zero "
                  "at and beyond the radius (brush/Deposit.hpp), so a tip this size deposits "
                  "nothing anywhere. Raise the brush size first.",
                  static_cast<double>(tip.radius));
    r.error = buf;
    return r;
  }
  if (!anyShapeHasGeometry(shapes)) {
    r.error =
        "stroke path refused: the path is empty -- no subpath has two anchors, so there is no "
        "line for the brush to follow. Add at least two anchors to a subpath.";
    return r;
  }

  // The brush's footprint reaches `tip.radius` either side of the centreline,
  // so the on-canvas test is the PATH's bounds outset by the radius -- not
  // `clipForPath()`, which is the fill's question. A path just off the left
  // edge with a 60 px brush still paints.
  {
    PathBounds b = shapePathBounds(shapes);
    PathBounds grown = b;
    if (b.valid) {
      grown.minX -= tip.radius; grown.minY -= tip.radius;
      grown.maxX += tip.radius; grown.maxY += tip.radius;
    }
    if (boundsMissCanvas(grown, width, height)) {
      r.error = "stroke path refused: the path lies entirely outside the canvas, further off "
                "it than the brush radius reaches back, so no dab would land -- " +
                boundsSentence(b, width, height) + " Move the path onto the canvas first.";
      return r;
    }
  }

  // --- The dabs. `brush/StrokePath` is the emitter; there is no second one.
  const float spacingPx = tip.spacingPx();
  std::vector<Vec2> dabs;
  StrokePath emitter;

  for (const VectorShape& shape : shapes) {
    if (pathIsEmpty(shape.path)) continue;
    const std::vector<FlatContour> contours =
        flattenPath(shape.path, kPathConsumerTolerancePx);
    for (const FlatContour& contour : contours) {
      if (contour.points.empty()) continue;
      // Per contour, not per stroke: leftover arc length and the four-sample
      // point history carried across would let one contour's tail curve into
      // the next one's head, laying dabs in the GAP between two subpaths the
      // user drew apart.
      //
      // **Redundant today, and kept anyway -- measured, not assumed.** A
      // sabotage that deleted this line reddened NOTHING, and the reason is
      // that `StrokePath::flush()` clears `numPts_`, `leftover_` and
      // `movedPx_` on every one of its exit paths, so the flush at the bottom
      // of this loop has already reset the emitter before the next contour
      // starts. That is an internal detail of brush/StrokePath, not a promise
      // its header makes, and the failure if it ever changed is a stroke that
      // joins two separate subpaths -- so the line stays, and this comment
      // records that it is belt and braces rather than load-bearing. The
      // assertion that covers the property is real and is reddened by
      // deleting the `flush()` below instead.
      emitter.reset();
      for (const PathPoint& p : contour.points) emitter.addPoint(p.x, p.y, spacingPx, dabs);
      // A closed contour does not repeat its first point (core/PathFlatten.hpp),
      // so the closing edge is walked by feeding it once more.
      if (contour.closed)
        emitter.addPoint(contour.points.front().x, contour.points.front().y, spacingPx, dabs);
      emitter.flush(spacingPx, dabs);
    }
  }

  if (dabs.empty()) {
    r.error =
        "stroke path refused: the path produced no dabs. Every subpath flattened to nothing, "
        "which happens when a subpath's anchors and handles are all at one point.";
    return r;
  }
  r.dabs = dabs.size();

  // --- The deposit. Both routes already exist and take the same arguments.
  size_t texels = 0;
  if (rgbTarget) {
    RgbStroke stroke;
    // `target.alphaLocked` is threaded in rather than refused --
    // PathConsumers.hpp section 5. brush/RgbDeposit.hpp section 4.5 is the
    // composite it selects.
    //
    // `tip.blend` is threaded in too, for the reason a live RGB stroke passes
    // it (app/StrokeSession.cpp's `StrokeSession::begin()`): the brush's own
    // blend mode is applied to a stroke on an RGB layer, and stroking a path
    // with that brush is a stroke on an RGB layer. This call used to omit it,
    // so the defaulted `BlendMode::Normal` painted a Multiply brush as Normal
    // -- white ink over grey whitened it -- while the Tool Options banner said
    // the mode was applied. `--selftest`'s path consumers section 10 pins it.
    // The pigment branch below takes no blend, for `BrushTip::blend`'s stated
    // reason (a Pigment texel has no RGBA to blend).
    stroke.begin(tip.linearRgb, tip.opacity, target.alphaLocked, tip.blend);
    const StrokeDeposit d =
        stroke.depositDabs(*target.rgbTiles, tip, dabs, width, height, selection);
    texels = d.texels;
    stroke.end();
  } else {
    const StrokeDeposit d =
        depositDabs(*target.pigmentTiles, tip, dabs, width, height, selection);
    texels = d.texels;
  }

  if (texels == 0) {
    r.error =
        "stroke path refused: the stroke changed no texel. The dabs were emitted but every "
        "one of them landed where the active selection excludes painting, or off the canvas, "
        "or on texels the stroke's opacity ceiling had already reached.";
    return r;
  }

  r.ok = true;
  r.editLabel = "stroke path with brush";
  r.texelsChanged = texels;
  return r;
}

}  // namespace np
