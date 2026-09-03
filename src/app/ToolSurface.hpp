#pragma once

#include "app/AppState.hpp"

// app/ToolSurface -- the SECOND axis of "is this palette cell live":
// docs/testing-issues.md **T5**, "closing every document leaves a canvas
// belonging to nothing."
//
// ==========================================================================
// 0. The two questions, and why conflating them is the bug
// ==========================================================================
//
// `ui/AtelierChrome`'s pair already answers the first one:
//
//   `toolImplemented(t)`        -- is this tool BUILT?
//   `toolHasCanvasHandler(t)`   -- and does the canvas actually listen to it?
//
// `--selftest` asserts those two are equal for every `Tool`, and the reason
// that assertion exists is the eyedropper: `implemented = true` for two whole
// phases with no handler anywhere, so the palette drew a live cell, the cursor
// promised a pick, and the click reached a canvas that ignored it.
//
// **That pair says nothing at all about the SURFACE.** Both are properties of
// the build, fixed at compile time. Neither knows whether there is anything in
// front of the user for the handler to act ON -- and with no document open
// there very often is not:
//
//   * `sim::PaintSim` owns a single dense GPU texture with no layer awareness
//     (PLAN.md Phase 4 step 8; `app/DocumentLifecycle.hpp`'s "what is still not
//     wired"). It is a real, paintable surface and it is not a document.
//   * `AppState::documents` can be, and on a fresh launch is, empty.
//
// So a Marquee cell with no document open is `toolImplemented()`, is
// `toolHasCanvasHandler()`, draws live, draws selectable, takes the click,
// takes the accent fill -- and its handler then reads
// `st.documents.active()`, gets `nullptr`, and installs nothing. That is the
// eyedropper's defect exactly, one axis over: every tier of the chrome says
// live except the one that acts. This header is that axis.
//
// ==========================================================================
// 1. Where the line is drawn, and why it is drawn THERE
// ==========================================================================
//
// **The bare canvas is a supported workflow and this module must not disable
// it.** File > "New" was deliberately renamed "New Canvas" precisely so it
// could not be read as "New Document" -- painting with no document open is
// designed behaviour, not a hole. What T5's short-term half asks for is that
// the *document-scoped* features stop pretending, not that the canvas stop
// working.
//
// The line therefore falls exactly where the tools' OWN gates already put it,
// and it is derived from those gates rather than from a list kept here:
//
//   ACT WITH NO DOCUMENT           because
//   ----------------------------   -----------------------------------------
//   Hand, Zoom                     they move `AppState::view`, which is
//                                  process state. There is no document in the
//                                  expression at all.
//   Measure                        `MeasureLine::documentId` is 0 both when
//                                  there is no document and when
//                                  `measureLineAppliesTo()` asks -- that
//                                  header says so outright, and the ruler is
//                                  geometry over the canvas, not over a layer.
//   Brush, Water, Dry Brush        `strokeRouteFor(t, nullptr)` answers
//                                  `StrokeRoute::PaintSim`. Its own comment:
//                                  "no target at all is the one case the
//                                  solver canvas is right for". This is the
//                                  workflow above, and it is the one thing
//                                  this module must not break.
//
//   NEED A DOCUMENT                because
//   ----------------------------   -----------------------------------------
//   Eraser, Pencil, Smudge,        `strokeRouteFor(t, nullptr)` answers
//   Clone Stamp, Dodge, Burn       `None` for each of them, by name, in that
//                                  function's own `target == nullptr` row: the
//                                  solver has no alpha, no aliased mark, no
//                                  smudge, nothing to sample and no tonal
//                                  step, so sending any of them there would
//                                  run the PAINT path and do something else
//                                  entirely.
//   Paint Bucket, Gradient         `pixelOpRefusalFor(nullptr)` is
//                                  `PixelOpRefusal::NoLayer`, which is that
//                                  enum's own first non-`None` row.
//   Marquee, Ellipse Marquee,      their handler is written
//   Lasso, Polygon Lasso,          `installSelection(*od, ...)`: a `Selection`
//   Magic Wand                     is bounded by a document's width and height
//                                  and lives on `OpenDocument`. There is no
//                                  nullptr form of the call to answer.
//   Move                           `beginMove(st.transform, *od)` takes the
//                                  document by reference. Structurally
//                                  impossible, not merely refused.
//   Eyedropper                     `applyEyedropperPick()`'s own first branch:
//                                  "The solver canvas is not a document and
//                                  `probePixel()` takes one, so there is
//                                  genuinely nothing to sample."
//
// **This is a strict subset of `toolImplemented()`, and that is the evidence
// it is a real axis rather than a synonym.** Six tools of the twenty-one this
// build has behaviour for survive with no document; the other fifteen do not,
// and the seven unbuilt cells are outside this question entirely (see §2).
// `--selftest` asserts the subset relation and the strictness both ways, so a
// future edit that quietly collapses this predicate into either of the
// existing two reddens rather than passing.
//
// ==========================================================================
// 2. What this module deliberately does NOT answer
// ==========================================================================
//
// **Not the long-term half of T5.** The canvas-to-document bridge -- which
// layer the solver deposits into, and a texture-to-tile path with its own
// dirty tracking -- is a separate, large piece of work (PRD O6). Nothing here
// touches `sim::PaintSim`'s ownership; this module only makes the state the
// build already has legible.
//
// **Not the refusal ladder.** "Locked layer", "wrong layer kind", "alpha
// locked" and "no anchor yet" are answered by `strokeRouteFor()`,
// `pixelOpRefusalFor()` and `cloneSourceRefusal()`, at the moment of the
// gesture, naming the layer. Re-deriving any of them here would be a second
// copy to drift, and would answer a question the palette cannot ask -- the
// palette is drawn once per frame for twenty-eight tools, not per gesture for
// one. This axis is exactly the one thing the palette CAN know: whether there
// is a document behind the canvas at all.
//
// **Not "is it built".** For a tool with no canvas handler in any state -- the
// seven `toolImplemented() == false` cells -- the surface is not what is wrong
// with it, and `toolSurfaceRefusal()` answers `nullptr` rather than stacking a
// second reason on top of "Not built yet." Two axes, two sentences, never both
// at once.

namespace np {

// Whether `tool`'s canvas handler can do anything at all with **no document
// open** -- see §1 for the six that can and the argument for each.
//
// Derived, never listed. Every term below is a call to the gate the
// corresponding canvas block is actually written with, asked with the
// no-document argument those gates already accept, so a tool that gains or
// loses a no-document route is picked up here for free:
//
//   `toolPansView()` / `toolZoomsView()`  -- take no document to begin with
//   `toolMeasuresCanvas()`                -- `MeasureLine`'s documentId 0 row
//   `strokeRouteFor(tool, nullptr)`       -- the route table's own nullptr row
//   `pixelOpWritesLayer(nullptr)`         -- `PixelOpRefusal::NoLayer`
//
// The three families with no term are the three where "no document" is not a
// value the call can take at all: the selection tools, Move and the
// eyedropper each dereference an `OpenDocument`, so there is no nullptr answer
// for a term to ask for. §1's table records that, and `--selftest` pins each
// of them so that a future no-document form of any one of them fails here
// rather than silently disagreeing with the palette.
//
// Not `noexcept`: `strokeRouteFor()` is, but this function is declared
// alongside `toolSurfaceRefusal()` below, which reaches `toolBeginsStroke()`
// (two probe `Layer`s, a `std::string` and an `optional<TileStore>` each), and
// a pair of predicates that differ in that one qualifier reads as an accident
// rather than a decision.
bool toolActsWithoutDocument(Tool tool);

// The sentence a palette cell shows when the SURFACE is what refuses `tool`,
// or `nullptr` when it does not.
//
// `nullptr` in three separate cases, and the third one matters:
//
//   1. `documentOpen` -- the surface is a document; whatever else may refuse
//      the gesture is the refusal ladder's business, not this axis's (§2).
//   2. `toolActsWithoutDocument(tool)` -- the tool is one of §1's six.
//   3. **`tool` has no canvas handler at all.** "Not built yet." is already
//      the whole answer for those seven cells; a second sentence underneath it
//      would be telling a user to open a document so that a tool which does
//      not exist can fail to act on it.
//
// **The voice is the build's existing one, verbatim, and deliberately not a
// new one.** `ui/MacPaintUI.cpp:350` and `:5352` already end with
// "no document is open. File > New Document makes one." -- that clause is
// reused unchanged. What varies is only the lead-in noun, exactly as those two
// sites already vary theirs ("layer command refused:" against "Nothing to
// sample:"), and it is chosen by which GATE the tool answers rather than by
// the tool's identity, so a sixth stroke tool inherits a sentence instead of
// needing a row added for it.
const char* toolSurfaceRefusal(Tool tool, bool documentOpen);

}  // namespace np
