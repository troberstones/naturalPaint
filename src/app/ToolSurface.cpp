#include "app/ToolSurface.hpp"

#include "app/CropTool.hpp"       // toolCropsCanvas()
#include "app/MoveTool.hpp"       // toolMovesPixels()
#include "app/PenTool.hpp"        // toolEditsPath()
#include "app/ShapeTool.hpp"      // toolCreatesShapes()
#include "app/RegionTool.hpp"     // toolCreatesRegions() -- Frame and Slice, PLAN.md gap-closing wave
#include "app/StrokeSession.hpp"  // the route table and five of the gates
#include "app/TextTool.hpp"       // toolEditsText()
#include "app/ZoomAndSize.hpp"    // toolZoomsView()

namespace np {

bool toolActsWithoutDocument(Tool tool) {
  // The view pair. `AppState::view` is process state -- a pan and a zoom are
  // the same operation with a document and without one -- so these two take no
  // document to begin with and there is nothing to ask them about it.
  if (toolPansView(tool) || toolZoomsView(tool)) return true;

  // The ruler. `app/MeasureLine.hpp`'s `documentId` comment is the whole
  // argument and it is explicit: "0 is what `OpenDocument` itself defaults to
  // and is also the value used when there is no open document, which is
  // deliberate ... `measureLineAppliesTo()` is asked against the same 0, so the
  // two agree." A measurement is geometry over the canvas; it writes no texel
  // and reads no layer.
  if (toolMeasuresCanvas(tool)) return true;

  // The stroke family, asked of the route table itself rather than restated
  // -- with one term subtracted from what that table alone would say.
  // `strokeRouteFor()`'s `target == nullptr` row still answers `PaintSim` for
  // Brush, Water and Dry Brush ("no target at all is the one case the solver
  // canvas is right for") and `None` for the eraser, the pencil, the tonal
  // pair, the clone and the smudge, each by name. That table is about which
  // LAYER a stroke can reach, not about whether `sim::PaintSim` currently
  // exists to reach at all -- and docs/testing-issues.md T5 (reversed
  // 2026-09-08) means it no longer does with no document open: the canvas is
  // torn down with the last document rather than left standing for anyone to
  // paint on. So `PaintSim` on its own is no longer enough to survive here;
  // only a route that lands on an actual Layer does.
  const StrokeRoute route = strokeRouteFor(tool, nullptr);
  if (route != StrokeRoute::None && route != StrokeRoute::PaintSim) return true;

  // The fill family, asked the same way. `pixelOpRefusalFor(nullptr)` is
  // `PixelOpRefusal::NoLayer` today, so this term is false for the bucket and
  // the gradient -- and it is written as a term rather than omitted precisely
  // because it is the one of the four document-requiring families that COULD
  // acquire a no-document answer without touching this file: give the fills a
  // canvas route and this flips on its own.
  if (toolWritesRgbPixels(tool) && pixelOpWritesLayer(nullptr)) return true;

  // Everything else. Three of the remaining families -- the five selection
  // tools, Move, and the eyedropper -- have no term here because they have no
  // no-document *form*: `installSelection(*od, ...)`, `beginMove(ts, *od)` and
  // `probePixel(od->document, ...)` all take the document by reference or by
  // dereference, so there is no nullptr answer for a term to ask for. The
  // header's §1 table records that reasoning per family, and
  // `--selftest` pins each of the three, so a future no-document form of any
  // one of them fails an assertion here rather than quietly disagreeing with
  // what the palette draws.
  //
  // The seven cells with no canvas handler at all fall out here too, and
  // correctly: a tool that cannot act on ANY surface cannot act on this one.
  // `toolSurfaceRefusal()` below is what keeps that from being SAID twice.
  return false;
}

const char* toolSurfaceRefusal(Tool tool, bool documentOpen) {
  if (documentOpen || toolActsWithoutDocument(tool)) return nullptr;

  // One clause, shared and verbatim: `ui/MacPaintUI.cpp:350` and `:5352` both
  // end "no document is open. File > New Document makes one." A second
  // phrasing of the same fact would be a second thing to keep in step, and the
  // instruction a user has to carry out is identical in every row below.
  //
  // The ladder is over GATES, not over `Tool` values, so it is seven rows for
  // twenty-five tools and a further tool in any family inherits its family's
  // sentence. Ordered cheapest-first for the reason `toolHasCanvasHandler()`
  // orders its own disjunction: `toolBeginsStroke()` builds two probe `Layer`s
  // and is last, so no tool that answers an earlier gate ever reaches it.
  if (toolDrawsSelection(tool))
    return "Nothing to select: no document is open. File > New Document makes one.";
  if (toolSamplesCanvas(tool))
    return "Nothing to sample: no document is open. File > New Document makes one.";
  if (toolMovesPixels(tool))
    return "Nothing to move: no document is open. File > New Document makes one.";
  if (toolWritesRgbPixels(tool))
    return "Nothing to fill: no document is open. File > New Document makes one.";
  // The sixth row, and it earns its own lead-in rather than borrowing Move's:
  // what a crop does to a document is change its extent, and "Nothing to move"
  // would name the wrong operation for a user reading it off a dimmed cell.
  // The ladder is over gates and this is the eighth gate (app/CropTool.hpp
  // §6). `Tool::Slice` shares Crop's palette group but answers its OWN gate
  // (`toolCreatesRegions()`, below) rather than this one -- sharing a palette
  // slot never meant sharing a capability, and now that Slice is built
  // (PLAN.md gap-closing wave) it earns its own row rather than silently
  // falling through to "Not built yet."
  if (toolCropsCanvas(tool))
    return "Nothing to crop: no document is open. File > New Document makes one.";
  // The ninth gate: `Tool::Frame` and `Tool::Slice` (app/RegionTool.hpp),
  // built on top of `core::Document::regions` rather than on Crop's. Its own
  // lead-in: a region is neither a selection, a move nor a crop, and
  // "Nothing to crop" off a dimmed Frame cell would misname what the tool
  // does.
  if (toolCreatesRegions(tool))
    return "Nothing to mark out: no document is open. File > New Document makes one.";
  // Its own lead-in rather than borrowing Move's or the stroke row's: the Pen
  // does not paint and does not move pixels, it edits geometry -- and a user
  // reading "Nothing to paint on" off a dimmed Pen cell would be told about
  // the wrong operation. Placed before `toolBeginsStroke()` because that row
  // builds two probe `Layer`s and this one is a table lookup.
  if (toolEditsPath(tool))
    return "Nothing to draw paths in: no document is open. File > New Document makes one.";
  // Its own lead-in for the Pen's reason: a user reading "Nothing to draw
  // paths in" off a dimmed Text cell would be told about the wrong operation,
  // and the Text tool neither paints nor edits anchors -- it sets type.
  if (toolEditsText(tool))
    return "Nothing to set type in: no document is open. File > New Document makes one.";
  // Its own lead-in for the identical reason: Shape neither paints, moves
  // pixels, nor edits an existing anchor model -- it drags out a new
  // primitive -- so "Nothing to draw paths in" (the Pen's own row) would
  // name a tool this cell is not.
  if (toolCreatesShapes(tool))
    return "Nothing to draw shapes in: no document is open. File > New Document makes one.";
  if (toolBeginsStroke(tool))
    return "Nothing to paint on: no document is open. File > New Document makes one.";

  // No gate at all, so no canvas handler at all: `toolImplemented()` is
  // already the whole answer for this cell and `toolTooltip()` already appends
  // "Not built yet." Adding a second sentence underneath would tell a user to
  // open a document so that a tool which does not exist can fail to act on it.
  // Two axes, two sentences, never both at once -- see the header's §2.
  return nullptr;
}

}  // namespace np
