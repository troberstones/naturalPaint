#pragma once

#include "app/CanvasView.hpp"  // CanvasView

namespace np {

// Track `split`, View > Match Zoom: given the focused pane's own view and
// the two documents' pixel sizes, the companion pane's view that shows the
// SAME zoom factor and the SAME point of the document at the pane's centre.
//
// **"Relative position" means normalised centre in document coordinates.**
// `source` puts the point `(srcW/2 - panX/zoom, srcH/2 - panY/zoom)` -- the
// document-space point currently at the viewport's own centre, from
// `ViewTransform`'s identity-rotation algebra -- at a fraction
// `(0.5 - panX/(zoom*srcW), 0.5 - panY/(zoom*srcH))` of the document. Placing
// that same fraction of `dstW`x`dstH` at ITS pane's centre, at the identical
// `zoom`, and solving for the destination pan collapses to one line per axis:
// `dstPan = srcPan * (dstSize / srcSize)`. No document size ever multiplies
// out to zero in the numerator, so the only degenerate input is `srcW`/`srcH`
// <= 0, guarded below by returning `source` unchanged (nothing to be
// relative to).
//
// Rotation and the two mirrors are NOT mirrored to the companion -- a
// decision, not an omission. The companion pane never rotates or flips its
// own quad (see ui/MacPaintUI.cpp's companion-pane block), so there is no
// second `ViewTransform` for a rotated point to travel through; carrying
// `source.rotation` into a view that is never drawn rotated would be a value
// nothing reads. `zoom`/`panX`/`panY` are the only fields this touches;
// every other `CanvasView` field is copied from `source` so the companion
// keeps riding along with `grayscale`/`grade` exactly as it does today.
CanvasView matchZoomView(const CanvasView& source, float srcW, float srcH, float dstW,
                         float dstH) noexcept;

}  // namespace np
