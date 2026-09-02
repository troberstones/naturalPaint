#pragma once

#include <cstdint>

// app/MeasureLine -- the ruler the Measure tool drags, and what it reports.
//
// ==========================================================================
// 0. The one tool in the palette that writes NOTHING
// ==========================================================================
//
// Every other built tool ends in a texel: the brush family deposits, the two
// fill ops write, the selection tools build a `Selection`, the eyedropper
// writes a foreground colour, the hand and the zoom write the view transform.
// Measure ends in a *sentence*. You drag a line across the picture and the
// options bar tells you how long it is and which way it points; the document
// is byte-identical before and after, and so is the view.
//
// That is why this module is `app/` and not `brush/` or `ops/`. It is not a
// `StrokeRoute` -- `app/StrokeSession.hpp` §1's table is about strokes that
// reach a Layer, and this gesture reaches none, so `strokeRouteFor()` keeps
// answering `None` for `Tool::Measure` and that answer is now a decision
// rather than a not-built placeholder. It is not a `pixelOpRefusalFor()`
// client either: a ruler has no target layer to be locked, no store to be
// missing, and therefore nothing to refuse. **A measurement is legal on an
// empty document, on a locked layer, on an Adjustment layer and with no
// layer selected at all**, and building it through either of those paths
// would have invented a refusal the tool cannot have.
//
// **It records no history entry**, for the reason `ui/MacPaintUI.cpp`'s
// eyedropper block already argues one level up: history is for edits to the
// document, `core::History` has no field a measurement could be stored in,
// and undoing "past" a measurement would have to restore a ruler position
// the entry does not carry. A ruler that piled a row into PRD O2's panel per
// drag would make the panel useless for finding the edit the user came to
// undo.
//
// ==========================================================================
// 1. Where the line lives, and why it carries a document id
// ==========================================================================
//
// **Not a local in the canvas block.** The whole value of the tool is that
// the line outlives the gesture: you drag it, you let go, and *then* you read
// the numbers. A local would be gone by the frame the user looked at the
// options bar. So the line is `AppState::measure` -- beside `marqueeX0..Y1`
// and `lassoPoints`, which are the other in-flight canvas gestures, and for
// the same reason those are there.
//
// **App-scoped, with the document it was measured on recorded on it.** The
// tempting alternative is to hang it off `OpenDocument`, one ruler per
// document, and it was rejected: an `OpenDocument` is what gets saved
// (`io/NpaintFile`), snapshotted into `core::History` and compared byte for
// byte by `--selftest`, so putting a transient tool readout on it would make
// a measurement a document mutation -- the exact thing §0 says this tool
// never does.
//
// But app-scoped alone is a lie of a different kind. The coordinates are
// **document texels of one particular document**, so a line dragged on a
// 4000 px scan still sitting in `AppState` while the user tabs to a 512 px
// sketch would report a length that has nothing to do with what is on
// screen. That is the stale-`marqueeX0..Y1` defect `ui/MacPaintUI.cpp`'s
// lasso branch was written about, one panel over and with numbers instead of
// a rectangle -- numbers being *worse*, because a wrong rectangle is visible
// and a wrong measurement is a plausible integer.
//
// `documentId` plus `measureLineAppliesTo()` is the whole fix: the drawing
// code and the readout both ask whether this line is about the document in
// front of them, and a line that is not simply does not show. Cheaper than
// clearing on every tab switch and, unlike clearing, it survives a round
// trip back to the document the measurement was actually taken on.
//
// `uint64_t` rather than `DocumentId` (`app/DocumentLifecycle.hpp`) so this
// header stays pure geometry -- `<cstdint>` and nothing else -- instead of
// pulling `core/Document` and `io/NpaintFile` in for one integer. The alias
// is pinned to that spelling by a `static_assert` in this module's `.cpp`,
// so the two cannot drift apart silently.
//
// ==========================================================================
// 2. Units: document texels, and nothing else
// ==========================================================================
//
// `lengthPx` is in **document texels**, not screen pixels, so a measurement
// does not change when the user zooms -- which is the only reading under
// which the number means anything about the picture. `ui/MacPaintUI.cpp`'s
// canvas block already hands every other tool the same `tx`/`ty` (the
// pointer, mapped back through `ViewTransform`'s real inverse), so this
// costs no conversion of its own and cannot disagree with where the line is
// drawn.
//
// **Not inches, not centimetres, and that is a gap and not a decision made.**
// Photoshop's Info panel converts through the document's resolution; this
// build's `core::Document` carries no DPI field, so there is nothing to
// convert *with*. Stating it here so a later reader adds the field rather
// than inventing a 72 dpi constant at this call site.
//
// ==========================================================================
// 3. The angle convention -- reused, never re-derived
// ==========================================================================
//
// `app/selftest/AngleConvention.cpp` exists because this codebase has been
// bitten by angle conventions, and its section 1 pins the answer for this
// whole build: **positive is CLOCKWISE on screen, measured from due east**,
// because `+y` is down in every raster here. `ops/Gradient.hpp`'s `Angular`
// and `ops/Transform.hpp`'s `transformRotateDegrees()` each derive the same
// sense independently.
//
// So `measureReadout()` does not call `atan2` at all. It calls
// **`dynamicDirection()`** (`brush/Dynamics.hpp`) -- the function that
// already turns a travel vector into a heading for the DIRECTION dynamic,
// and the one `AngleConvention.cpp` section 2 pins geometrically against
// `dabCoverage()`'s own rotation. Two consequences, both wanted:
//
//   * There is exactly ONE vector-to-heading function in the build. A second
//     one here, even a correct one, is a second place for the sign to be
//     wrong and a second thing to keep in step with `brush/Deposit`.
//   * `--selftest` can pin the ruler's angle the same way `AngleConvention`
//     pins the brush's: feed the reported angle into a `BrushTip` and ask
//     `dabCoverage()` whether the footprint actually reaches a point along
//     the measured line. That is a geometric answer, not this file's
//     arithmetic restated back at itself.
//
// The dependency direction (`app/` on `brush/`) is deliberate and is the
// cheaper of the two: the alternative is to lift the heading into a third
// module that both depend on, which moves `AngleConvention.cpp`'s pinned
// function out from under the test that pins it.
//
// **Range `[0, 360)`, not `(-180, 180]`.** That is `dynamicDirection()`'s own
// range and the reason it gives is the reason here too: a heading has no
// rest orientation to be signed about. Photoshop's Info panel shows the
// signed form; matching it would mean either a second convention in this
// build or a presentation-layer negation, and if a later step wants the
// signed reading it belongs in the options bar's `printf`, where it is
// obviously a display choice, and not in this struct, where it would look
// like a fact about the geometry.
//
// ==========================================================================
// 4. The degenerate drag
// ==========================================================================
//
// A click with no movement is a real gesture -- it happens every time a user
// taps the canvas to see where the tool is -- so it must produce a *number*,
// not a NaN and not a blank. `lengthPx` is 0 and `angleDeg` is 0, and the
// second one is inherited rather than special-cased: `std::atan2(0, 0)` is
// `0` by IEEE754 contract, which is the case `dynamicDirection()`'s own
// header already argues for a stroke's first dab. There is deliberately no
// `if (lengthPx == 0)` branch here, because a branch would be a second
// answer to a question that already has one.
//
// The trap this avoids by construction is the normalise-then-acos shape --
// `acos(dx / lengthPx)` -- which looks like the obvious way to get an angle,
// divides by zero on exactly this gesture, and produces a NaN that then
// prints as `nan` in the options bar and propagates into anything that ever
// arithmetic on it.
//
// ==========================================================================
// 5. What this is not
// ==========================================================================
//
// **No protractor.** Photoshop's ruler can grow a second arm (Option-drag
// from an endpoint) and report the angle between them. Two arms is a second
// gesture, a second pair of endpoints and a second readout layout; one arm
// is what `docs/ui.md` section 2's palette cell asks for and what PRD names.
// Recorded so a later reader adds the arm on purpose rather than discovering
// its absence.
//
// **No endpoint dragging.** A line is replaced by starting a new drag, not
// adjusted by grabbing an end. Same argument: hit-testing endpoints is a
// separate gesture with its own cursor states, and a ruler is cheap enough
// to simply redraw.
//
// **Not persisted.** It is not in the `.npaint` and not in the journal. A
// measurement is a question the user asked once, not a property of the
// picture.

namespace np {

// The ruler, in document texel space. One per application -- see §1.
struct MeasureLine {
  // A line exists: draw it, report it. Stays true after the pointer comes
  // up, which is the entire point of the tool (§1).
  bool active = false;
  // The pointer is still down: the far end tracks it. Distinct from `active`
  // rather than folded into it, because "there is a line" and "the line is
  // still being drawn" are different questions and the second one is what
  // stops a stray pointer move from silently rewriting a finished ruler.
  bool dragging = false;
  // Which document's texels `x0..y1` are in -- `app/DocumentLifecycle.hpp`'s
  // `DocumentId`, spelled as its underlying type (§1). 0 is what
  // `OpenDocument` itself defaults to and is also the value used when there
  // is no open document, which is deliberate: with no document there is no
  // canvas to have measured and `measureLineAppliesTo()` is asked against
  // the same 0, so the two agree.
  uint64_t documentId = 0;
  float x0 = 0.0f, y0 = 0.0f;
  float x1 = 0.0f, y1 = 0.0f;
};

// What the line says. Every field is derived; nothing here is stored.
struct MeasureReadout {
  // The signed run and rise, in document texels: `x1 - x0` and `y1 - y0`.
  // Surfaced rather than left internal because the options bar shows W and H
  // beside L and A -- the same four numbers Photoshop's Info panel shows for
  // a ruler -- and because a caller that recomputed them from the endpoints
  // would be a second subtraction to get the sign wrong in.
  float dx = 0.0f;
  float dy = 0.0f;
  // Euclidean length of (dx, dy), in document texels. Never negative, never
  // NaN. See §2 and §4.
  float lengthPx = 0.0f;
  // Degrees in [0, 360), clockwise-positive from due east. See §3.
  float angleDeg = 0.0f;
};

MeasureReadout measureReadout(const MeasureLine& line) noexcept;

// Whether `line` is a measurement OF the document that is currently in front
// of the user. False for an inactive line, and false for one taken on a
// different document -- see §1 for why the second half is not paranoia.
bool measureLineAppliesTo(const MeasureLine& line, uint64_t activeDocumentId) noexcept;

// --- the gesture ----------------------------------------------------------
//
// Three functions rather than one `update(mouseDown, x, y)`, so that
// `--selftest` can drive the gesture headlessly in the same three steps
// `ui/MacPaintUI.cpp`'s canvas block calls them in, and so that the canvas
// block reads as the three events it actually has (clicked / held /
// released) rather than as a state machine spelled inline.

// Pen-down: both ends at (x, y), so a click that never moves is already a
// well-formed zero-length line (§4) rather than a line with an uninitialised
// far end.
void beginMeasureLine(MeasureLine& line, uint64_t documentId, float x, float y) noexcept;

// Pen-move: the far end follows. **A no-op unless `dragging`** -- that guard
// is what makes a finished ruler survive the pointer wandering across the
// canvas afterwards, which is exactly the frame in which the user is reading
// the numbers.
void updateMeasureLine(MeasureLine& line, float x, float y) noexcept;

// Pen-up: the drag ends, the line stays.
void endMeasureLine(MeasureLine& line) noexcept;

// Throw the ruler away. Called when the user leaves the tool -- a line drawn
// with Measure means nothing under the brush, and leaving it on screen would
// be furniture the user has no gesture to remove.
void clearMeasureLine(MeasureLine& line) noexcept;

}  // namespace np
