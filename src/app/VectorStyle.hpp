#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "app/PenTool.hpp"  // PathSelection, and section 3's two modes
#include "core/VectorShape.hpp"

// app/VectorStyle -- what the Pen paints with, and the one rule that decides
// whether a style control edits the SELECTION or the tool's default.
//
// ==========================================================================
// 1. WHY THIS EXISTS AT ALL: a pen-drawn path was invisible
// ==========================================================================
//
// `pathEditBeginPen()` built its shape as a default-constructed
// `VectorShape`, and `Paint::on` defaults to FALSE on both the fill and the
// stroke (core/VectorShape.hpp says why that default is right for the struct:
// SVG's `fill="none"` is a real state, distinct from an alpha of zero). So
// every path the Pen has ever drawn rasterised to nothing --
// `core/VectorRaster.cpp`'s two gates are `if (shape.fill.on)` and
// `if (shape.stroke.on && shape.strokeStyle.width > 0)`, and neither was ever
// true. The editing overlay drew the path anyway, which is exactly why this
// never LOOKED broken: it vanished the moment the tool changed.
//
// The Text tool one screen over already had the answer and stated the rule
// this violated -- a new object takes the TOOL's current style plus the
// FOREGROUND colour (`ui/MacPaintUI.cpp`'s frame-drag end). This header is
// the Pen's half of that rule.
//
// ==========================================================================
// 2. STROKE ON, FILL OFF -- the defaults, and the argument for them
// ==========================================================================
//
// A pen is a LINE. Someone drawing with it is laying down a contour, and the
// contour is what they expect to see.
//
// Filling by default is worse than merely unexpected: a fill is only defined
// over an enclosed region, so filling an OPEN path means the rasteriser
// implicitly closes it (SVG's own rule for `fill` on an open subpath). The
// user would then see a straight edge they never drew, joining their last
// anchor back to their first, appearing and moving with every further click.
// That is a surprise the control cannot explain, so the default is off and
// the FILL swatch's [NONE] chip is where a user says otherwise.
//
// `strokeStyle.width` stays `StrokeStyle`'s own 1.0 rather than a thicker
// "nicer" default: one document pixel is the width every other vector
// producer in this tree (io/SvgImport's initial value, `StrokeStyle` itself)
// already means by "unspecified", and inventing a second answer here would
// make a Pen path and an imported path with no `stroke-width` differ for no
// reason a user could name.
namespace np {

// The style the NEXT pen-drawn shape gets. Stored on `AppState` beside
// `textStyle`, which is the exact precedent: the style of the next authored
// object, editable before there is an object to edit.
//
// Colour is NOT stored for the initial stroke -- see `penVectorStyle()` in
// `ui/MacPaintUI.hpp`, which fills `stroke.rgba` from
// `foregroundLinearRgba()` at the call site, the way the Text tool's does.
// This struct carries whatever the options bar last set, and the foreground
// overwrites it on the way into a new shape only when the user has not.
struct VectorStyle {
  // `Paint::on` defaults false -- section 2's argument.
  Paint fill;
  // ...and this one is deliberately the other way round.
  Paint stroke{/*on=*/true, {0.0f, 0.0f, 0.0f, 1.0f}};
  StrokeStyle strokeStyle;
};

// Read a shape's three style members out as one, and write them back. The
// pair exists so that "the selection's style" and "the default style" are
// the same TYPE at every call site -- a control that had to branch on which
// of the two it was editing is a control with two chances to edit the wrong
// one, which is the defect section 3 is about.
VectorStyle vectorStyleOf(const VectorShape& shape);
void setVectorStyle(VectorShape* shape, const VectorStyle& style);

// **What a colour swatch does to a paint**: sets the colour, turns the paint
// on, and makes it SOLID.
//
// The third part is the one worth a function. `Paint` gained a `kind` with
// docs/psd-vector-shapes.md's S2, and a gradient paint IGNORES `rgba`
// entirely (core/VectorShape.hpp) -- so a swatch that wrote only `rgba` and
// `on` would leave a gradient-filled shape painting its gradient while the
// swatch showed the colour the user had just picked. That is precisely the
// "a control with no visible effect" failure the STROKE swatch's own comment
// is about, arriving through a field that did not exist when it was written.
//
// Dropping the gradient is the right answer rather than a regrettable one: a
// user who opens the colour picker on a shape and chooses a colour is asking
// for that colour. The gradient's table entry survives (entries are never
// erased -- core/Gradient.hpp), so undo restores the fill intact.
void setPaintSolidColor(Paint& paint, const std::array<float, 4>& linearRgba);

// ==========================================================================
// 3. SELECTION FIRST, ELSE DEFAULT
// ==========================================================================
//
// docs/path-editing-plan.md section 2.2, stated once here so both the STROKE
// and the FILL controls follow the same rule and neither has to restate it:
//
//   > If the current `PathSelection` names one or more shapes -- in EITHER
//   > mode, since Component mode's anchors resolve to the shapes they belong
//   > to -- the control edits THOSE SHAPES and the caller records an edit.
//   > Otherwise it writes the default.
//
// The consequence that has to be designed for rather than discovered: the
// control must SHOW the selection's value, not the default's, whenever a
// selection exists. A control that displays one value while editing another
// is a control that lies about what the next drag will change.

// The shapes a style control would edit, in LAYER order (the order of
// `shapes`, not of the selection), de-duplicated, and containing only ids
// that actually exist. Empty means "no target" -- the default is written
// instead.
//
// Layer order rather than selection order because the readout below shows
// "the first" of a mixed selection, and a readout that reordered itself when
// the user shift-clicked in a different sequence would be reporting the
// click history rather than the picture.
std::vector<uint64_t> vectorStyleTargets(const std::vector<VectorShape>& shapes,
                                         const PathSelection& selection);

// What a style control SHOWS.
struct VectorStyleReadout {
  // The first target's style, or `fallback` when there is no target.
  VectorStyle style;
  // True when `style` came from the selection. The controls use it to label
  // what they are about to change.
  bool fromSelection = false;
  size_t targetCount = 0;
  // The "mixed" affordance, per field rather than one flag for the row: two
  // shapes can agree about their stroke colour and disagree about its width,
  // and a single flag would smear a tilde across a control that has exactly
  // one honest value to report.
  bool mixedStrokeWidth = false;
  bool mixedStrokeOn = false;
  bool mixedStrokeColor = false;
  bool mixedFillOn = false;
  bool mixedFillColor = false;
};

VectorStyleReadout vectorStyleReadout(const std::vector<VectorShape>& shapes,
                                      const PathSelection& selection,
                                      const VectorStyle& fallback);

// A control's change, expressed once against a `VectorStyle&` so it can be
// applied to a shape or to the default without the caller writing it twice.
using VectorStyleMutator = std::function<void(VectorStyle&)>;

// Applies `mutate` per section 3's rule.
//
// **Returns the number of SHAPES it changed.** Zero means the selection named
// none and `*fallback` was written instead -- which is also the caller's cue
// NOT to record a document edit, because nothing in the document moved. A
// bool would have answered "did it work", which is not the question any
// caller has.
size_t applyVectorStyleEdit(std::vector<VectorShape>* shapes, const PathSelection& selection,
                            VectorStyle* fallback, const VectorStyleMutator& mutate);

}  // namespace np
