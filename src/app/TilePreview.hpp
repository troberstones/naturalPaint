#pragma once

#include <cstddef>

namespace np {

struct CanvasView;  // app/AppState.hpp

// app/TilePreview -- PRD D8, PLAN.md Phase 9 ("Tile it"): the 3x3 repeat
// preview.
//
// PRD.md:211 is unusually blunt about the status of this one -- "a
// **requirement**, not a nicety -- tileability cannot be judged from a single
// tile" -- and the reason is that the defect it hunts lives *at the document's
// own edge*. A seam is a discontinuity between the right column of pixels and
// the left one, and no view of a single tile can put those two columns next to
// each other. Laying the document out nine times does, eight different ways at
// once (four edges and four corners), which is the whole of what this module
// is for.
//
// **There is no engine work here and there is deliberately none.** The pixels
// already exist; this is a *view* of them. Nothing in this file allocates,
// composites, resamples or copies anything -- it answers two questions for
// `ui/MacPaintUI.cpp`'s canvas block ("where do the copies go" and "how big is
// the field they cover") and owns the enter/leave bookkeeping for the one
// piece of state a toggle like this cannot avoid having. Everything is pure
// and headless, no ImGui and no GPU, for the reason `app/ZoomAndSize.hpp`'s
// own header gives at length: a suite that asserts on the constants a drawing
// block reads, rather than on the functions it calls, stays green through a
// regression of the code that reads them.
//
// ==========================================================================
// 1. Nine copies, one transfer function
// ==========================================================================
//
// The copies are drawn by the *same* `addCanvasQuad()` call the single tile
// has always gone through, once per offset -- never `ImDrawList::AddImageQuad()`.
// `ui/CanvasQuad.hpp` carries the full argument; the short version is that
// the canvas composite holds **linear light**, ImGui's pipeline would apply a
// second decode to it, and linear 0.25 would reach the screen as byte 61
// instead of 137. The error is zero at both endpoints, so a preview drawn the
// wrong way looks entirely plausible and is wrong everywhere in between -- and
// it would be wrong *differently* in the eight repeats than in the centre if
// only one of the two paths were used, which is precisely the kind of
// edge-to-edge mismatch this preview exists to make visible. One path, nine
// quads.
//
// `tilePreviewTiles()` therefore returns offsets in *document* coordinates,
// not screen coordinates: the caller maps all nine through the one
// `ViewTransform` it already built, so the repeats zoom, pan, mirror and
// rotate *with* the document rather than beside it.
//
// **The offsets come back with the centre LAST.** The eight repeats are
// subordinate and the document is the subject; drawing the subject last means
// no repeat can ever land on top of it, whatever a future caller inserts
// between the two.
//
// ==========================================================================
// 2. What the preview deliberately does NOT draw
// ==========================================================================
//
// A dimmed outer ring was considered and rejected. It is the obvious way to
// say "the centre one is your document", and it would destroy the measurement:
// every centre-to-neighbour boundary would acquire a step in luminance, and a
// luminance step across the document's edge is *exactly* the artefact the user
// has opened this preview to look for. A tool that manufactures the defect it
// is meant to detect is worse than no tool.
//
// So the nine tiles are drawn identically and the document is marked by the
// one piece of chrome the canvas already had: the border `ui/MacPaintUI.cpp`
// draws around the document quad, which under the preview reads as the seam
// indicator -- it is drawn *along* the boundary being judged, one pixel wide,
// and it changes none of the pixels on either side of it. No new chrome was
// invented for this.
//
// The drop shadow is the one thing that does move: it belongs behind the whole
// field rather than behind the centre tile, or it would cut a dark band
// through the middle of the picture. `tilePreviewField()` is what the caller
// asks for that rectangle.
//
// ==========================================================================
// 3. It is a view, so it does not change what a click means
// ==========================================================================
//
// Painting is untouched. The pointer maps back through the same
// `ViewTransform` it always did, so a click on the centre tile paints exactly
// where it did before the preview was opened, and a click on one of the eight
// repeats resolves to a document coordinate outside `[0, w) x [0, h)` and
// deposits nothing -- the same thing clicking the surround has always done at
// a zoomed-out view. That is a deliberate choice and not an omission: making
// a click on a repeat wrap into the document would mean the pointer no longer
// paints under itself, which is a different and much larger feature (a
// wrapped-canvas painting mode) than "show me whether this tiles".
//
// The preview follows the document, because it is the document's own texture
// drawn nine times: an edit inside it appears in all nine copies on the next
// frame, with no invalidation of its own, which is the property that makes it
// usable while healing a seam rather than only after.
//
// ==========================================================================
// 4. Entering and leaving give the view back
// ==========================================================================
//
// Entering the preview has to re-fit, or the user sees the same view scaled
// and eight tiles off the edge of the window -- which shows less than they had
// before, not more. Leaving has to give back the view they were in, the way
// `docs/ui.md:205`'s spring-loaded tools give back the tool you had rather
// than dropping you on one you never chose.
//
// **Exactly three fields are saved and restored: zoom, panX and panY**, and
// the reason to be precise about it is that restoring more would be a bug.
// Mirror, rotation, grayscale and grade are *not* touched by entering the
// preview, so a user who flips one while the preview is up flipped it
// deliberately; putting it back on the way out would revert an edit this
// feature never made. `resetCanvasView()`'s header makes the same
// field-by-field argument for the same reason.
//
// The re-fit itself is not performed here: the fit needs the canvas window's
// on-screen size, which only exists inside `ui/MacPaintUI.cpp`'s
// `Begin()`/`End()` block. `setTilePreview()` raises the *existing*
// `AppState::requestFitWindow` flag instead, and the fit block divides by
// `tilePreviewSpan()` -- so there is one fit-to-window computation in this
// application, not a second one that could drift from it.

// Three copies per axis, and only three. Not a preference: three is the
// smallest number that puts a neighbour on *both* sides of the document along
// each axis, which is what makes all four edges and all four corners visible
// at once. Five would show the same eight adjacencies again, smaller.
inline constexpr int kTilePreviewSpan = 3;
inline constexpr size_t kTilePreviewMaxTiles =
    static_cast<size_t>(kTilePreviewSpan) * static_cast<size_t>(kTilePreviewSpan);

// The preview's whole state. Lives on `AppState` beside `CanvasView` rather
// than inside it: `CanvasView` is composed into `app/ViewTransform`'s affine
// matrix and every field in it is an input to that matrix, which none of these
// are -- this changes how many times the transform is applied, never what it
// is.
struct TilePreviewState {
  bool active = false;
  // The view to give back on the way out. `hasSaved` distinguishes "saved a
  // zoom of 1.0" from "never entered the preview", which a bare 1.0 could not.
  bool hasSaved = false;
  float savedZoom = 1.0f;
  float savedPanX = 0.0f;
  float savedPanY = 0.0f;
};

// One copy's position in the field, in whole document widths/heights from the
// document itself. `{0, 0}` is the document; `{-1, 0}` is the repeat to its
// left.
struct TileOffset {
  int col = 0;
  int row = 0;
};

// The field's rectangle in document coordinates -- `{0, 0, w, h}` with the
// preview off, the 3x3 block around it with the preview on.
struct TileFieldRect {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
};

// Fills `out` with the copies to draw and returns how many. **One** when the
// preview is off, and that one is `{0, 0}` -- so the caller's draw loop with
// the preview off is byte-for-byte the single-quad code path it replaced,
// rather than a second arrangement that has to be kept in step with it.
// Nine when the preview is on, the eight repeats first and `{0, 0}` last.
size_t tilePreviewTiles(const TilePreviewState& tile,
                        TileOffset out[kTilePreviewMaxTiles]) noexcept;

// How many documents wide the drawn field is: 1 off, `kTilePreviewSpan` on.
// The fit-to-window computation divides by this.
int tilePreviewSpan(const TilePreviewState& tile) noexcept;

// The field's extent, for the drop shadow (see section 2).
TileFieldRect tilePreviewField(const TilePreviewState& tile, float texW, float texH) noexcept;

// Turn the preview on or off, saving the view on the way in and giving it back
// on the way out (section 4). `requestFit` is only ever set *true* -- it is
// `AppState::requestFitWindow`, a request another part of the frame consumes,
// and clearing it here would swallow a Fit to Window the user asked for in the
// same frame. Setting the preview to the state it is already in does nothing
// at all, so a menu tick that is redrawn every frame cannot slowly overwrite
// the saved view with the preview's own.
void setTilePreview(TilePreviewState& tile, CanvasView& view, bool& requestFit, bool on) noexcept;

}  // namespace np
