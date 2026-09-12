#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/Gradient.hpp"
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
// A paint is a KIND, and a gradient lives in the document's table
// ==========================================================================
//
// `Paint` carries a colour and, since docs/psd-vector-shapes.md's S2, a
// `PaintKind` plus an index into `core::Document::gradients`.
//
// **The heavy data is at the table and not in the shape**, which is the
// decision this section exists to record. A `GradientStops` is two heap
// vectors; a `Paint` is a member of `VectorShape`, which is a member of
// `core::Layer`, which `core::History` snapshots BY VALUE on every edit. Two
// vectors per paint per shape per undo step is a cost nobody would choose,
// and it also destroys the authored fact that an SVG file's `url(#g)` on
// forty shapes is ONE gradient -- core/Gradient.hpp's `GradientDef` states
// both halves of that argument, and io/SvgImport.hpp section 3 asked for
// exactly this shape before it existed.
//
// **What this header does NOT include, and the trade that changed.** The
// model used to sit in ops/Gradient.hpp, whose own includes pull
// core/TileStore.hpp and core/SelectionMask.hpp in behind it -- and a
// `VectorShape` is reachable from `core::Layer`, so that would have landed in
// nearly every translation unit in the build. So the ramp model split into
// core/Gradient.hpp, which includes `<array>`, `<string>` and `<vector>` and
// nothing else, and this header includes that. It is the same type
// ops/Gradient's `renderGradient()` evaluates, not a second copy: a gradient
// drawn with the Gradient tool and a gradient filling a vector shape cannot
// interpolate differently.
//
// **A pattern fill still has nowhere to land**, and `PaintKind` deliberately
// has no `Pattern` member. An enum value with no producer and no renderer is
// reachable the moment anyone writes one (see this project's own
// absence-claim traps), and the honest state is that io/PsdVectorStyle names
// `PtFl` in a warning and leaves the fill off. Adding the member belongs with
// the work that can populate and paint it.
//
namespace np {

// What a `Paint` paints WITH. `Solid` reads `rgba`; `Gradient` reads
// `gradient` and ignores `rgba` entirely.
enum class PaintKind : uint8_t {
  Solid = 0,
  Gradient = 1,
};

// A paint, or none at all.
//
// `on == false` is genuinely different from an alpha of zero: SVG's
// `fill="none"` means the shape has no fill *at all*, which matters because a
// shape with no fill and no stroke still exists, still hit-tests for
// selection, and still round-trips.
struct Paint {
  bool on = false;
  // Linear-light, straight alpha. See this header's section 1. **Read only
  // when `kind == Solid`**; a gradient paint carries whatever value happened
  // to be here and it means nothing.
  std::array<float, 4> rgba{0.0f, 0.0f, 0.0f, 1.0f};

  PaintKind kind = PaintKind::Solid;

  // Index into `core::Document::gradients`, meaningful only when
  // `kind == Gradient`.
  //
  // **An index past the end of the table paints NOTHING.** Not black, not
  // `rgba`, not the last entry -- nothing, and core/VectorRaster is where that
  // is enforced. The alternative, falling back to `rgba`, is the failure mode
  // this project's refusal discipline is built against: a document that opens
  // without error and renders a colour nobody authored. An out-of-range index
  // is a bug in whoever built the shape, and it must look like one.
  uint32_t gradient = 0;
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
// **`gradients` is not optional, and that is the point.** A gradient paint's
// appearance lives in the table, so a hash over the shapes alone would be
// blind to an edit of the ramp they reference: the stops change, the hash does
// not, and core/VectorRaster hands back the stale raster it already has. That
// is exactly the invisible-edit failure this function exists to prevent,
// reintroduced one level out. Only the entries a shape actually references are
// hashed, so adding an unreferenced gradient to a document does not
// re-rasterise every vector layer in it.
uint64_t vectorContentHash(const std::vector<VectorShape>& shapes,
                           const GradientTable& gradients) noexcept;

// The union of every shape's tight bounds, including the outset a stroke adds
// (half the stroke width, plus the miter allowance where a miter join can
// reach further than that). Conservative: never smaller than the drawn area,
// which is what a caller allocating tiles needs.
PathBounds vectorShapesBounds(const std::vector<VectorShape>& shapes) noexcept;

}  // namespace np
