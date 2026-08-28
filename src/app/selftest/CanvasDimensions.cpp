#include "app/selftest/Support.hpp"

#include "app/ZoomAndSize.hpp"

namespace np {

// naturalPaint canvasdim bug fix: `canvasDimensionsFor()` (app/ZoomAndSize.
// hpp section 4) is the one function `ui/MacPaintUI.cpp`'s canvas block now
// calls for its `texW`/`texH` -- everything that block draws or hit-tests
// against (fit-to-window, `drawSize`, the `ViewTransform`, the corner quad,
// the navigator, the zoom anchor, hit-testing, the grid overlay and guides)
// reads those two numbers, so this is the one place the reported bug
// ("Making a non-square document still shows up square. Changing the canvas
// size to non-square shows square.") can be caught headlessly -- the canvas
// block itself is unreachable from `--selftest` (docs/reachability-audit.md
// F4: no ImGui frame, no window).
bool runCanvasDimensionsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ==========================================================================
  // (a) A non-square document's own size comes back exactly -- the bug's own
  // reproduction, in both orientations.
  // ==========================================================================
  {
    OpenDocument portrait = makeBlankOpenDocument(800, 1200, WorkingSpace{}, "portrait");
    const CanvasDimensions p = canvasDimensionsFor(&portrait, 1024, 1024);
    check(p.w == 800.0f && p.h == 1200.0f,
          "canvasDimensionsFor: portrait document (800x1200) returns its own size, "
          "not the square 1024x1024 fallback");

    OpenDocument landscape = makeBlankOpenDocument(1600, 900, WorkingSpace{}, "landscape");
    const CanvasDimensions l = canvasDimensionsFor(&landscape, 1024, 1024);
    check(l.w == 1600.0f && l.h == 900.0f,
          "canvasDimensionsFor: landscape document (1600x900) returns its own size");

    // A document that genuinely IS the fallback's own size is not a
    // degenerate case for this function -- it just happens to agree with
    // the fallback, and this checks the document branch is actually being
    // taken (not, say, a bug where the fallback is returned unconditionally
    // and every other case in this section is coincidentally still right
    // because 800x1200 and 1600x900 both differ from it already).
    OpenDocument square = makeBlankOpenDocument(1024, 1024, WorkingSpace{}, "square");
    const CanvasDimensions s = canvasDimensionsFor(&square, 640, 480);
    check(s.w == 1024.0f && s.h == 1024.0f,
          "canvasDimensionsFor: a document matching the fallback's own size "
          "still reads from the document, not the fallback (640x480)");
  }

  // ==========================================================================
  // (b) No document open -- the session-empty fallback.
  // ==========================================================================
  //
  // `nullptr` is not a corner case a caller has to guard against separately:
  // `DocumentSession::active()` on an empty session already returns it, and
  // `ui/AtelierChrome.hpp`'s `atelierPaneDocuments()` agrees (its own "one
  // empty pane" comment) -- so this is the literal argument
  // `ui/MacPaintUI.cpp`'s canvas block passes on that frame.
  {
    const CanvasDimensions d = canvasDimensionsFor(nullptr, 1024, 1024);
    check(d.w == 1024.0f && d.h == 1024.0f,
          "canvasDimensionsFor: no document (nullptr) falls back to kCanvasW/kCanvasH");

    const CanvasDimensions d2 = canvasDimensionsFor(nullptr, 640, 480);
    check(d2.w == 640.0f && d2.h == 480.0f,
          "canvasDimensionsFor: the fallback is whatever the caller passes, "
          "not a hardcoded 1024x1024 read a second time");
  }

  // ==========================================================================
  // (c) After a resize -- the second half of the bug report ("Changing the
  // canvas size to non-square shows square").
  // ==========================================================================
  //
  // `ops/DocumentTransform.cpp`'s `resizeDocumentCanvas()`/
  // `resizeDocumentImage()` both mutate `Document::width`/`height` in place
  // (that .cpp's own code -- `doc.width = width` inside `Document::
  // createBlank()`'s sibling paths, and `cropDocument()`'s effect on an
  // existing `Document&`); this reproduces that shape directly on the field
  // rather than pulling in the whole resize pipeline, since what is under
  // test here is only "does canvasDimensionsFor() read the CURRENT width/
  // height", not the resize arithmetic itself (ops/DocumentTransform.cpp's
  // own suite owns that).
  {
    OpenDocument doc = makeBlankOpenDocument(1024, 1024, WorkingSpace{}, "resizeme");
    const CanvasDimensions before = canvasDimensionsFor(&doc, 1024, 1024);
    check(before.w == 1024.0f && before.h == 1024.0f,
          "canvasDimensionsFor: before resize, a 1024x1024 document reads 1024x1024");

    doc.document.width = 500;
    doc.document.height = 1300;
    const CanvasDimensions after = canvasDimensionsFor(&doc, 1024, 1024);
    check(after.w == 500.0f && after.h == 1300.0f,
          "canvasDimensionsFor: after Canvas Size to 500x1300, the SAME OpenDocument* "
          "reads the new size on the next call -- no stale copy anywhere to go out "
          "of step with the document it was taken from");
  }

  // ==========================================================================
  // (d) The defensive zero/negative-dimension fallback.
  // ==========================================================================
  //
  // Unreachable through any constructor this build ships today -- `Document::
  // createBlank()` and every `io::` loader enforce a positive width and
  // height before a `Document` value can exist at all (this function's own
  // header comment) -- but every division in the canvas block's
  // fit-to-window and zoom-anchor arithmetic (`avail.x / texW`, etc.)
  // ultimately depends on `canvasDimensionsFor()` never handing back a zero,
  // so this checks the guard directly rather than trusting that no future
  // caller ever constructs a malformed `Document` a different way.
  {
    OpenDocument zeroW = makeBlankOpenDocument(1, 1, WorkingSpace{}, "zero");
    zeroW.document.width = 0;
    const CanvasDimensions z = canvasDimensionsFor(&zeroW, 1024, 768);
    check(z.w == 1024.0f && z.h == 768.0f,
          "canvasDimensionsFor: a zero-width document falls back rather than "
          "handing fit-to-window a division by zero");

    OpenDocument negH = makeBlankOpenDocument(1, 1, WorkingSpace{}, "neg");
    negH.document.height = -5;
    const CanvasDimensions n = canvasDimensionsFor(&negH, 1024, 768);
    check(n.w == 1024.0f && n.h == 768.0f,
          "canvasDimensionsFor: a negative-height document falls back the same way");
  }

  // ==========================================================================
  // (e) The solver's size is budgeted; the display's is not.
  // ==========================================================================
  //
  // `ui/MacPaintUI.cpp` hands the active document's size to TWO places that
  // take the same-shaped argument and pay wildly different prices for it: the
  // canvas quad (free) and `ensurePaintSim()` (`sim/PaintSim.cpp`'s
  // `allocFields()`, 176 bytes per texel unconditionally and 272 once
  // `allocInkFields()` has run). `paintSimDimensionsFor()` is the second one's
  // guard, and these are the assertions that would catch someone later
  // "simplifying" the two call sites back into one -- which is exactly the
  // shape the canvasdim fix arrived in, and which turns the first brush-down
  // on a large document into a multi-gigabyte allocation request.
  {
    // The bug report's own document. 800 x 1200 = 960,000 texels, inside the
    // 1,048,576 budget, so it keeps its own shape -- the whole point of the
    // budget being a status-quo cap rather than a round number is that the
    // reported case still gets fixed.
    OpenDocument portrait = makeBlankOpenDocument(800, 1200, WorkingSpace{}, "portrait");
    const CanvasDimensions p = paintSimDimensionsFor(&portrait, 1024, 1024);
    check(p.w == 800.0f && p.h == 1200.0f,
          "paintSimDimensionsFor: the reported 800x1200 (960,000 texels) is inside "
          "the budget and keeps its own shape");

    // Exactly at the budget: the size this build has always allocated. The
    // boundary is inclusive, and it has to be, or the fix would change the
    // solver's size for the default document.
    OpenDocument atCap = makeBlankOpenDocument(1024, 1024, WorkingSpace{}, "atcap");
    const CanvasDimensions a = paintSimDimensionsFor(&atCap, 640, 480);
    check(a.w == 1024.0f && a.h == 1024.0f,
          "paintSimDimensionsFor: 1024x1024 is exactly kPaintSimMaxTexels and is "
          "ACCEPTED (inclusive bound -- the default document must not be clamped)");

    // One texel column over. Chosen deliberately over a comfortably-large
    // document: an off-by-one in the comparison (`<` for `<=`, or the budget
    // read as a dimension rather than an area) survives a 4000x3000 probe and
    // dies here.
    OpenDocument justOver = makeBlankOpenDocument(1025, 1024, WorkingSpace{}, "justover");
    const CanvasDimensions j = paintSimDimensionsFor(&justOver, 1024, 1024);
    check(j.w == 1024.0f && j.h == 1024.0f,
          "paintSimDimensionsFor: 1025x1024 is one column over the budget and falls "
          "back to the caller's own size");

    // An ordinary photo import -- the case a user reaches without trying. At
    // 272 B/texel this is 3.3 GB, against the 285 MB this build has ever
    // actually allocated.
    OpenDocument photo = makeBlankOpenDocument(4000, 3000, WorkingSpace{}, "photo");
    const CanvasDimensions ph = paintSimDimensionsFor(&photo, 1024, 1024);
    check(ph.w == 1024.0f && ph.h == 1024.0f,
          "paintSimDimensionsFor: an ordinary 4000x3000 import (3.3 GB of solver "
          "fields) falls back rather than being allocated");

    // The preset maximum (app/DocumentPresets.hpp's kMaxDocumentPresetDimension
    // = 32768). 1.07 Gtexel x 272 B = 292 GB. This is the assertion that says
    // the guard is not decorative.
    OpenDocument huge = makeBlankOpenDocument(32768, 32768, WorkingSpace{}, "huge");
    const CanvasDimensions hu = paintSimDimensionsFor(&huge, 1024, 1024);
    check(hu.w == 1024.0f && hu.h == 1024.0f,
          "paintSimDimensionsFor: the 32768x32768 preset maximum (292 GB of solver "
          "fields) falls back rather than being allocated");

    // And the two functions genuinely disagree on that document -- the display
    // quad still follows it exactly. A "fix" that clamped BOTH would quietly
    // reintroduce the reported bug for large documents, and this is the only
    // assertion here that would notice.
    const CanvasDimensions hugeDisplay = canvasDimensionsFor(&huge, 1024, 1024);
    check(hugeDisplay.w == 32768.0f && hugeDisplay.h == 32768.0f,
          "the solver's budget does NOT clamp the display: the same 32768x32768 "
          "document still draws at its own size");

    const CanvasDimensions none = paintSimDimensionsFor(nullptr, 1024, 1024);
    check(none.w == 1024.0f && none.h == 1024.0f,
          "paintSimDimensionsFor: no document falls back exactly as the display "
          "path does");
  }

  return ok;
}

}  // namespace np
