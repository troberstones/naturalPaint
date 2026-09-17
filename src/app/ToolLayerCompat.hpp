#pragma once

#include "app/AppState.hpp"

// app/ToolLayerCompat -- the FOURTH axis of "is this palette cell live".
//
// `ui/AtelierChrome`'s pair asks whether a tool is BUILT; `app/ToolSurface`
// asks whether there is a document behind the canvas at all;
// `app/ToolSwitch`'s modal lock asks whether a live transform gizmo has
// frozen the whole palette. None of the three asks the question this module
// answers: given the tool IS built, the surface IS a document and nothing is
// modal, can this tool do anything with the layer that is actually selected?
//
// **The defect this repairs.** A Vector or Text layer carries no `rgbTiles`
// store at all (core/Layer.hpp: both are that layer's ONLY content, and it is
// an invariant that no other kind populates them). Marqueeing a selection
// over one, or clicking the Magic Wand on one, installed a `Selection` or
// probed a store that the gesture's own handler (`pixelOpRefusalFor()`,
// `strokeRouteFor()`) refuses the instant a stroke or a fill is attempted --
// but the PALETTE said nothing. A user selecting a text layer and reaching
// for the wand got a live-looking cell that did nothing when clicked, which
// is the identical defect `app/ToolSurface` exists to prevent for "no
// document", one layer-kind over.
//
// **Derived from the same store test `pixelOpRefusalFor()` already uses, not
// from `kind ==` comparisons.** `rgbTiles.has_value()` is the codebase's
// established idiom for "is there an RGB pixel store here to act on" --
// Adjustment layers use it, Filters use it, Bucket/Gradient use it. A Vector
// or Text layer answers `false` for the same reason an Adjustment layer does:
// no store, not merely a store this build treats specially.
//
// **Deliberately scoped to the selection family plus Bucket/Gradient, and
// NOT the brush family.** That is a narrower list than "every tool
// `strokeRouteFor()` can refuse", and the narrowing is load-bearing rather
// than an oversight: `strokeRouteFor()`'s own Pigment-layer arm refuses the
// Pencil, Dodge, Burn, Clone Stamp and Heal there BY NAME while admitting the
// Brush and Dry Brush, which is a per-tool ladder this module would get
// wrong if it collapsed the whole family into one `rgbTiles.has_value()`
// question -- a Pigment layer has no `rgbTiles` (it holds `pigmentTiles`
// instead, core/Layer.hpp), so that collapse would silently disable Brush
// and Dry Brush on the one layer kind built to receive them. The tools named
// here all share the one property that makes the simpler question the RIGHT
// one: every one of them is written against `rgbTiles` alone, with no second
// store it could mean instead -- `pixelOpRefusalFor()`'s own two direct
// callers (Bucket, Gradient), and the selection family, whose
// `installSelection()` has no Pigment-specific arm at all. `--selftest`
// cross-checks this predicate against `pixelOpRefusalFor()` itself so the two
// cannot silently diverge.
//
// **What this module deliberately does NOT answer**, for `app/ToolSurface`'s
// own two reasons: it is not the refusal ladder (a locked RGB layer, an
// alpha lock, "no clone anchor yet" stay exactly where they are, answered at
// the gesture -- and so does every per-tool Pigment-layer rule above), and
// it is not "is it built" (the seven unimplemented cells are outside this
// question, same as they are outside `app/ToolSurface`'s).
//
// **Every other tool is valid on every kind, including nullptr.** Move,
// Crop, Hand, Zoom, Measure, the vector tools (Pen, Curve, PathSelect,
// Shape), Frame/Slice, the Text tool, the Eyedropper and the whole brush
// family are none of them refused by this axis -- the brush family's own
// per-kind rules live in `strokeRouteFor()`, where they already are, and
// disabling Pen because the active layer is not yet a Vector layer would
// make it impossible to ever start one.
namespace np {

// True if `tool` reads or writes a pixel store (`Layer::rgbTiles`) and
// `layer` does not carry one -- the one case this axis refuses. Every other
// combination, including `layer == nullptr`, answers true: "no active layer"
// is `app/ToolSurface`'s question, not this one.
bool toolNeedsPixelLayer(Tool tool) noexcept;

// Whether `tool` can act on `layer`. `layer` may be null.
bool toolValidForLayer(Tool tool, const Layer* layer) noexcept;

// The sentence a palette cell shows when the LAYER KIND is what refuses
// `tool`, or `nullptr` when `toolValidForLayer()` is true. Named after the
// layer the way `toolSurfaceRefusal()` is named after "no document" --
// distinguishable from every other refusal a hover can show, and never
// stacked with one: this only has an opinion once a document, a surface and
// a concrete layer all already exist.
const char* toolLayerRefusal(Tool tool, const Layer* layer);

}  // namespace np
