#pragma once

#include "app/CanvasView.hpp"     // CanvasView
#include "brush/StrokePath.hpp"  // Vec2

namespace np {

// Where a document's own top-left corner lands on screen, for a pane at
// `paneOrigin` sized `paneSize`, a `docW`x`docH` document, and `view`.
//
// The one margin-centering convention every pane on the canvas uses:
// centred (an equal margin on both sides) when the document at this zoom is
// SMALLER than the pane; offset by `panX`/`panY` from that centred position
// once it is LARGER. `ui/MacPaintUI.cpp`'s focused canvas block computes its
// own `origin` with exactly this arithmetic (`app/selftest/
// ZoomToSelection.cpp` lines 36-45 independently reconstructs the same
// formula to verify a different feature), and the companion pane's block
// now calls this function rather than repeating it -- one place, not two
// that can drift.
Vec2 splitPaneOrigin(Vec2 paneOrigin, Vec2 paneSize, float docW, float docH,
                     const CanvasView& view) noexcept;

// View > Match Zoom: given the focused pane's own view, both
// panes' on-screen sizes, and both documents' pixel sizes, the companion
// pane's view that shows the SAME zoom factor and has the SAME document
// point at ITS pane's own centre.
//
// **"Relative position" means the document-space point at the pane's
// centre, as a fraction of that document's own size -- not
// `0.5 - pan/(zoom*docSize)`.** That formula is only the point-at-centre
// while the document fits inside the pane (`splitPaneOrigin()`'s margin is
// positive); once `zoom*docSize` exceeds the pane -- the ordinary zoomed-in
// case -- the margin is zero and the true fraction is
// `(paneSize/2 - pan) / (zoom*docSize)` instead. Both pane sizes are
// therefore required inputs, not something the mapping can work around:
// there is no zoom/pan pair that reproduces "the same point at the centre"
// without knowing how big each pane is. The two forms agree exactly where
// they overlap (`(paneSize - zoom*docSize)/2` substituted for the general
// margin recovers the simpler formula), so this subsumes the old
// fits-inside-its-pane-only mapping rather than replacing it with something
// unrelated.
//
// Degenerate (either document has no extent) returns `source` unchanged.
//
// Rotation and the two mirrors are NOT mirrored to the companion -- a
// decision, not an omission. The companion pane never rotates or flips its
// own quad, so there is no second `ViewTransform` for a rotated point to
// travel through; carrying `source.rotation` into a view that is never drawn
// rotated would be a value nothing reads. `zoom`/`panX`/`panY` are the only
// fields this touches; every other `CanvasView` field is copied from
// `source` so the companion keeps riding along with `grayscale`/`grade`
// exactly as it does today.
CanvasView matchZoomView(const CanvasView& source, Vec2 srcPaneSize, float srcW, float srcH,
                         Vec2 dstPaneSize, float dstW, float dstH) noexcept;

// The companion pane's view for this frame. Match Zoom follows the focused pane;
// otherwise a view at the `zoom <= 0` "needs a fit" sentinel is fitted inside a
// 24 px inset and centred, once; otherwise the companion keeps its own view.
CanvasView companionViewForFrame(const CanvasView& current, bool matchZoom,
                                 const CanvasView& focused, Vec2 focusedPaneSize, float focusedW,
                                 float focusedH, Vec2 companionPaneSize, float companionW,
                                 float companionH) noexcept;

}  // namespace np
