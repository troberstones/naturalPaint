#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "app/PenTool.hpp"     // PathEditState, pathEditSelectShapes()
#include "app/VectorStyle.hpp"  // VectorStyle, setVectorStyle()
#include "core/VectorShape.hpp"

// app/ShapeTool -- Tool::Shape's headless geometry: a drag's two points plus
// a kind and its one parameter become a `VectorShape`, per docs/ui.md §4a
// ("a Shape tool is a gesture that emits a `VectorShape` into the layer the
// Pen already edits") and docs/vector-editing.md §2 (`Tool::Shape` "is a
// different kind of gap ... and does not go through anchor-level editing at
// all").
//
// ==========================================================================
// 1. Why this is not `app/PenTool`
// ==========================================================================
//
// The Pen places one anchor per press and builds a shape over many frames.
// Shape places every anchor of a closed primitive in one function call, from
// exactly two points -- so it has no `PathDragKind`, no open-placement
// session, and no anchor-level hit-testing: `app/PenTool.hpp` §2's own
// comment already says so ("`Tool::Shape` ... does not go through
// anchor-level editing at all"), and this file does not widen
// `toolEditsPath()` to include it, for the reason that header's §2 argues at
// length (a predicate widened for a tool with a different gesture shape hands
// that tool's clicks to a handler written for something else).
//
// The shape this file DOES have -- a two-point drag with Shift/Option
// modifiers building one piece of geometry every frame of the drag, for a
// preview, and once more at release, for the commit -- is `app/GradientTool`'s
// shape, not the Pen's. `gradientToolGeometry()` is this file's closest
// precedent: one function, called by both the preview and the commit, so the
// two cannot silently disagree (`app/GradientTool.hpp` §1).
//
// ==========================================================================
// 2. What is NOT here
// ==========================================================================
//
// No `AppState`, no ImGui, no undo. `ui/MacPaintUI.cpp`'s canvas block is the
// thin caller: it owns the drag session (`ShapeDrag`, below -- the gesture's
// own two-point state, `GradientDrag`'s exact shape and for the same reason
// that header's §3a states: a shared mutable flag is how the gradient tool
// went inert for its entire history), decides whether to auto-create a
// Vector layer (the Pen's own rule, reused verbatim rather than re-derived),
// assigns the shape's id from the layer's `nextShapeId`, stamps it with
// `penVectorStyle()` (`ui/MacPaintUI.hpp`) -- the SAME style a Pen-drawn path
// gets, because both are "a new vector object" and `app/VectorStyle.hpp` §1
// is the account of what happens when a shape is built with the paint left
// off -- and records ONE undo entry at release.
namespace np {

// This header is included BY `AppState.hpp` (which owns `ShapeToolState` and
// `ShapeDrag` as members), so including it back would be a cycle -- the same
// arrangement `app/PenTool.hpp` sits in for the identical reason. An opaque
// declaration is all `toolCreatesShapes()` needs; `ShapeTool.cpp` includes
// the real definition.
enum class Tool;

// Which primitive the next drag draws. Reordering is safe: nothing indexes
// this enum, `kShapeKinds` below is walked by value.
enum class ShapeKind {
  Rectangle,
  Ellipse,
  RoundedRect,
  Polygon,
  Line,
};

// The tool's own settings -- `GradientToolState`'s exact shape, and the
// identical argument for being a struct rather than loose `AppState` fields
// (a tool's settings are a group, and the second and third settings should
// widen a struct that already exists rather than scatter loose fields).
//
// `cornerRadius` means nothing outside `RoundedRect` and `sides` means
// nothing outside `Polygon` -- both are read only by `shapeToolGeometry()`'s
// matching `case`, so an unrelated kind carries a harmless, inert value
// rather than a variant that has to be discriminated twice (once by `kind`,
// once by which member is "active").
struct ShapeToolState {
  ShapeKind kind = ShapeKind::Rectangle;

  // Document pixels. Clamped to the drawn rectangle's own half-extent by
  // `svgRectPath()` (io/SvgPath.hpp), the same clamp a plain rectangle drag
  // with a large radius already relies on, so this file does not repeat it.
  float cornerRadius = 16.0f;

  // Clamped to >= 3 by `shapeToolGeometry()` -- a polygon of fewer sides is
  // not a shape this tool can draw, and refusing silently (clamping) rather
  // than refusing loudly is consistent with every other geometry function in
  // this codebase that degrades a degenerate INPUT rather than a degenerate
  // OUTPUT (`svgRectPath()`'s own resolution of a negative radius pair).
  int sides = 5;
};

// The gesture's own two-point state -- `GradientDrag`'s exact shape
// (`app/GradientTool.hpp` §3a), for the identical reason: giving it a shared
// flag like `marqueeDragging` is how the gradient tool went inert for its
// entire history. `ui/MacPaintUI.cpp` is this struct's only writer.
struct ShapeDrag {
  bool active = false;
  // Which document the live gesture belongs to -- `CropSession::doc`'s rule:
  // a drag begun on one tab means nothing on another, and is dropped rather
  // than applied to the wrong picture.
  uint64_t documentId = 0;
  // Document texels. Pen-down is (x0, y0); the live pointer, and pen-up, is
  // (x1, y1).
  float x0 = 0.0f, y0 = 0.0f;
  float x1 = 0.0f, y1 = 0.0f;
};

// One row per `ShapeKind`, walked by the options bar's KIND segment --
// `GradientKindRow`'s exact reason: the one list a control walks is also the
// one list `--selftest` can check for a missing row, rather than a combo
// silently offering four of five.
struct ShapeKindRow {
  ShapeKind kind;
  const char* label;
  const char* tip;
};
inline constexpr size_t kShapeKindCount = 5;
extern const ShapeKindRow kShapeKinds[kShapeKindCount];

// ==========================================================================
// 3. The geometry
// ==========================================================================
//
// One function, called by the live preview every frame of the drag and once
// more at release for the commit -- `gradientToolGeometry()`'s own argument,
// restated here because it is the whole reason this is a function rather
// than an inline block at each of those two call sites.
//
// `p0` is pen-down, `p1` is the live pointer or the release point.
// `shiftConstrain` is Shift -- a square/circle bounding box for every kind
// except Line, whose own meaning is "snap the drag angle to the nearest 45
// degrees" (there is no width/height to equalise for a single segment).
// `fromCenter` is Option/Alt -- `p0` becomes the shape's centre instead of
// one of its corners, and for Line, the segment's midpoint instead of one of
// its ends.
//
// **A plain click creates nothing.** `p0 == p1` (exact equality -- the same
// "held-still pointer" test `app/PenTool.cpp`'s `pathEditUpdate()` uses)
// returns a default-constructed `VectorShape`: an empty `Path`, `fill.on ==
// false`, `stroke.on == false`. The caller's cue to commit nothing is
// `pathIsEmpty(shape.path)` (core/Path.hpp) -- the same predicate that also
// catches a one-axis drag that leaves a rectangle or ellipse's OTHER
// dimension at zero (`svgRectPath()`/`svgEllipsePath()`'s own degenerate
// rule) and a polygon whose bounding box has collapsed to a line.
//
// The returned shape's `fill` and `stroke` are both left off, `id` is left
// at 0, and `pivot` is left at `nullopt` -- exactly the state
// `pathEditBeginPen()` builds its own new shape in, and for the identical
// reason (`app/VectorStyle.hpp` §1): the caller stamps the paint and assigns
// the id, once, in the one place that does both for every vector tool this
// build has.
VectorShape shapeToolGeometry(const ShapeToolState& tool, PathPoint p0, PathPoint p1,
                              bool shiftConstrain, bool fromCenter) noexcept;

// ==========================================================================
// 4. The commit -- one function, so "one shape, one undo entry" is structural
// ==========================================================================
//
// What one release of the drag did. `Empty` is a plain click or a drag that
// collapsed to nothing (§3) -- the caller's cue NOT to call `recordEdit()`,
// `pathEditBeginPen()`'s `Inert`-vs-everything-else split for the identical
// reason: an undo entry for an edit that changed nothing is the empty entry
// `app/PenTool.hpp` refuses to open anywhere in this file's family.
enum class ShapeCommitResult { Empty, Committed };

// Builds the geometry (`shapeToolGeometry()`, so the preview and the commit
// are the same computation run twice, `app/GradientTool.hpp` §1's rule), and
// when it is non-empty: stamps it with `style`, assigns it `*nextShapeId`
// (advancing the counter -- the layer's own `Layer::nextShapeId`, exactly the
// counter `pathEditBeginPen()` advances for a Pen-drawn shape, so an id is
// never reused within a layer regardless of which tool minted it), appends it
// to `*shapes`, and -- when `pathEdit` is not null -- installs it as the
// SHAPE-mode selection via `pathEditSelectShapes()`, replacing whatever was
// selected. That last step is why the caller passes the live
// `PathEditState`: it is the same function the PATHS panel uses to select
// what a verb just minted (`app/PenTool.hpp` §8's own comment on it), reused
// rather than a second "make this the selection" written here, and it is
// what leaves a freshly drawn shape ready for `Tool::PathSelect` to
// manipulate immediately (docs/ui.md §4a's requirement for this track).
//
// `pathEdit == nullptr` skips the selection step only -- the geometry is
// still built and appended -- which is what lets this be tested without a
// live editing session (a `PathEditState` with no selection is a fine stand-in
// everywhere except the one assertion this parameter serves).
ShapeCommitResult commitShapeTool(const ShapeToolState& tool, PathPoint p0, PathPoint p1,
                                  bool shiftConstrain, bool fromCenter, const VectorStyle& style,
                                  std::vector<VectorShape>* shapes, uint64_t* nextShapeId,
                                  PathEditState* pathEdit);

// The eleventh term in `toolHasCanvasHandler()` (`ui/AtelierChrome.cpp`) --
// its own predicate rather than a name folded into `toolEditsPath()`, for
// that header's §2 reason: the two tools' gestures are shaped differently,
// and a predicate widened to cover both would hand Shape's clicks to a
// handler written for anchor placement.
bool toolCreatesShapes(Tool t) noexcept;

}  // namespace np
