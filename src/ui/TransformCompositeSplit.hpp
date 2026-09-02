#pragma once

#include <cstddef>

#include "core/Document.hpp"

// Splitting the document composite around a LIVE Free Transform.
//
// While a transform session is live the canvas has to show three things, in
// this order: the layers BELOW the transformed one, then that layer's pixels
// at their new position (ui/TransformPreviewTexture's quad), then the layers
// ABOVE it. Before this file the canvas showed the whole document -- the
// transformed layer INCLUDED, still at its original position -- with the
// preview quad painted over the top of everything, so a drag showed the
// picture twice and always in front of layers that belong in front of it.
//
// **The document is never modified.** A transform is not committed until
// Return, and mutating the document during the drag would put an erase into
// the undo stack, bump the revision on every frame, and hand the recovery
// journal a document with a hole in it. So both halves are composites of
// *views* of the same unmodified document, built by flipping `visible` on a
// copy. core/Document's tile stores are shared_ptr slots with copy-on-write,
// so a copy costs a layer vector, not a pixel.
//
// **When the split is exact, and when it is not.** Porter-Duff `over` is
// associative, so compositing the above-layers by themselves and drawing that
// result over the below-result is EXACTLY compositing them all together --
// but only while every above-layer is a plain `over` of its own pixels. A
// layer that reads the backdrop (any non-`normal` blend, an Adjustment layer),
// or whose meaning depends on a neighbour the split separated it from (a
// clipped layer, a group member), does not survive being composited over
// transparent black instead of over the real picture.
// `transformSplitIsExact()` answers that question, conservatively; a caller
// that gets `false` is expected to fall back to drawing the preview over the
// whole composite, which is what this application did for every transform
// before the split existed. The layer is still hidden in that case -- showing
// the picture once, in front, beats showing it twice.

namespace np {

// True when `doc`'s layers above `layerIndex` can be composited in isolation
// and drawn over the below-half without changing a single texel of the
// result. Conservative: it answers the question above by refusing every
// construct that could read a backdrop, not by reasoning about whether this
// particular one happens to.
//
// Hidden layers above are ignored -- they contribute nothing to either
// arrangement, so they cannot make the two differ.
//
// `layerIndex` out of range returns false: there is no split to take.
bool transformSplitIsExact(const Document& doc, size_t layerIndex) noexcept;

// Whether any layer above `layerIndex` would draw anything at all. False for
// the overwhelmingly common case of transforming the top layer, which is what
// lets that case skip the second composite and the texture behind it
// entirely rather than paying for a transparent one.
bool anyVisibleLayerAbove(const Document& doc, size_t layerIndex) noexcept;

// `doc` with `layerIndex` hidden: everything the canvas should show EXCEPT
// the pixels the transform preview is drawing at their new position. Returned
// unchanged for an out-of-range index.
Document documentWithLayerHidden(const Document& doc, size_t layerIndex);

// `doc` with every layer at or ABOVE `layerIndex` hidden: the below-half of
// the three-way stack, the one the preview quad is drawn on top of. Note this
// is NOT `documentWithLayerHidden()` -- that one keeps the layers above, which
// is right for the fallback (one texture, preview in front) and wrong for the
// split (they would be composited twice, once under the quad and once over
// it). Getting these two confused is exactly what this file's `--selftest`
// section caught when it was first written.
Document documentWithLayersAtOrAboveHidden(const Document& doc, size_t layerIndex);

// `doc` with every layer at or below `layerIndex` hidden: the above-half, to
// be drawn over the preview quad. Returned with everything hidden for an
// out-of-range index, which composites to transparent -- the honest answer to
// "what is above a layer that does not exist".
Document documentWithLayersAtOrBelowHidden(const Document& doc, size_t layerIndex);

}  // namespace np
