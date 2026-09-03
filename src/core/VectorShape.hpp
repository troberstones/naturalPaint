#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/Path.hpp"
#include "core/PathStroke.hpp"

// core/VectorShape -- what a `LayerKind::Vector` layer actually holds.
//
// One shape is a path plus how to paint it: a fill, a stroke, and an optional
// clip. A Vector layer is an ordered list of them, painted bottom to top, and
// that list IS the layer's content -- there are no tiles (see core/Layer.hpp's
// `shapes` member for where the rasterised result lives instead, and why it is
// not here).
//
// ==========================================================================
// Colour is linear and straight, like everything else in core/
// ==========================================================================
//
// `Paint::rgba` is **linear-light, straight (un-premultiplied) alpha**, the
// same convention core/Composite and core/Clipboard use. An SVG file gives
// sRGB-encoded 8-bit values, so io/SvgImport decodes through color/Space on
// the way in and nothing downstream of this struct ever sees a display
// encoding. Storing what the file said and converting at paint time would put
// a colour-space decision inside the rasteriser, which is precisely where
// this codebase has repeatedly decided it must not live.
//
// ==========================================================================
// Solid paint only, deliberately, and how gradients arrive
// ==========================================================================
//
// `Paint` carries a colour and nothing else today. Gradients are real scope --
// they are in the SVG import set -- but they land with io/SvgImport, which is
// the first thing that can produce one, and this codebase already has a
// complete gradient model in ops/Gradient.hpp (`GradientKind`,
// `GradientSpread`, `GradientStops`) that should be reused rather than
// duplicated here.
//
// It is NOT reused *now* for one concrete reason: a `VectorShape` is reachable
// from `core::Layer`, so anything this header includes is included by nearly
// every translation unit in the build. ops/Gradient.hpp is not the right thing
// to put there for a field no caller can yet populate. When gradients land,
// the shape is a paint kind plus an index into a document-level gradient
// table, which keeps the heavy type at the table rather than in every layer.
// The serialised form carries a version in its own prefix (io/PathSerial), so
// adding that costs a version bump and no migration.
namespace np {

// A solid paint, or none at all.
//
// `on == false` is genuinely different from an alpha of zero: SVG's
// `fill="none"` means the shape has no fill *at all*, which matters because a
// shape with no fill and no stroke still exists, still hit-tests for
// selection, and still round-trips.
struct Paint {
  bool on = false;
  // Linear-light, straight alpha. See this header's section 1.
  std::array<float, 4> rgba{0.0f, 0.0f, 0.0f, 1.0f};
};

// One painted path.
struct VectorShape {
  Path path;

  Paint fill;
  Paint stroke;
  StrokeStyle strokeStyle;

  // An optional clip, in the same space as `path`. Coverage is multiplied by
  // the clip's coverage, which is why core/PathRaster emits spans rather than
  // writing a destination -- intersecting two coverages is then a multiply
  // over a row, with no intermediate image. SVG's `clip-path` maps onto this
  // directly; `mask` will too, once io/SvgImport can produce one.
  std::optional<Path> clip;

  // PRD/Stage 4's manipulator pivot, in the shape's own coordinates.
  //
  // **Document data, not session state**, and that is the decision that makes
  // "move a shape's pivot and have it stick" true rather than approximately
  // true: it is serialised with the shape and it moves through undo like any
  // other edit. `nullopt` means "use the centroid", which is the default the
  // manipulator shows before anyone has moved it -- distinct from a pivot
  // that a user has deliberately placed AT the centroid, which must survive
  // the shape later being edited into a different centroid.
  std::optional<PathPoint> pivot;

  // A stable identity for selection and for the Paths panel, unique within
  // its layer. Zero means "not yet assigned".
  uint64_t id = 0;

  // What the user sees in the Paths panel. Empty is normal -- SVG rarely
  // names a shape, and the panel falls back to a positional label.
  std::string name;
};

// A content hash over everything that affects the rasterised result.
//
// **This exists so that cache invalidation cannot be forgotten.** The obvious
// design is a `geometryRevision` counter that every mutation bumps, and the
// obvious failure of that design is a mutation site that does not bump it:
// the symptom is a stale raster, i.e. an edit that silently does not appear,
// which is the single sharpest hazard in this whole feature (see
// core/DirtyTiles.cpp's pass 1, which compares kind/ops/mask/storage presence
// and nothing else, so a pure geometry edit is invisible to it).
//
// Hashing the content instead makes the question "did this change?" answerable
// from the data rather than from a promise. It costs a walk over the anchors,
// which for a path of a few hundred anchors is microseconds against a
// rasterisation that is milliseconds -- so it is far below the work it guards.
//
// Floats are hashed by their bit pattern, so it is exact rather than
// approximate: two paths that differ in the last ulp hash differently and are
// re-rasterised, which is the safe direction.
uint64_t vectorContentHash(const std::vector<VectorShape>& shapes) noexcept;

// The union of every shape's tight bounds, including the outset a stroke adds
// (half the stroke width, plus the miter allowance where a miter join can
// reach further than that). Conservative: never smaller than the drawn area,
// which is what a caller allocating tiles needs.
PathBounds vectorShapesBounds(const std::vector<VectorShape>& shapes) noexcept;

}  // namespace np
