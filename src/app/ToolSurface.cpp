#include "app/ToolSurface.hpp"

#include "app/CropTool.hpp"       // toolCropsCanvas()
#include "app/MoveTool.hpp"       // toolMovesPixels()
#include "app/StrokeSession.hpp"  // the route table and five of the gates
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

  // The stroke family, asked of the route table itself rather than restated.
  // `strokeRouteFor()`'s `target == nullptr` row is the literal decision:
  // Brush, Water and Dry Brush answer `PaintSim` ("no target at all is the one
  // case the solver canvas is right for"), and the eraser, the pencil, the
  // tonal pair, the clone and the smudge each answer `None` there, by name,
  // with a paragraph apiece saying what running them on the solver would
  // silently do instead. Every one of those decisions reaches this predicate
  // without being repeated in it.
  if (strokeRouteFor(tool, nullptr) != StrokeRoute::None) return true;

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
  // The ladder is over GATES, not over `Tool` values, so it is six rows for
  // twenty-two tools and a sixth tool in any family inherits its family's
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
  // §6), so `Tool::Slice` -- which shares Crop's palette group -- correctly
  // does NOT inherit this sentence: it has no gate, so it is a not-built cell
  // and gets "Not built yet." instead, which is the one-reason rule holding.
  if (toolCropsCanvas(tool))
    return "Nothing to crop: no document is open. File > New Document makes one.";
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
