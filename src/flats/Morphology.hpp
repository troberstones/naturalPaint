#pragma once

#include "flats/Field.hpp"

// flats/Morphology -- the binary-mask primitives everything else is built on.
// Ported from autoFlats src/core/morphology.ts.

namespace np {

// Chamfer 3-4 distance transform. mask: 1 = obstacle (distance 0). Returned
// distances are in units of 3 ≈ 1 pixel. Two raster passes, so O(N).
FlatDist flatDistanceTransform(const FlatMask& mask, int w, int h);

// Zhang-Suen thinning to a 1px 8-connected skeleton (returns a new mask).
// Erodes strokes to their centreline so downstream stages see uniform 1px
// lines -- gap size and width matching stop depending on how thick the artist
// drew, and fills grow up to the centreline (no fringe). A 4-connected flood
// cannot cross an 8-connected skeleton, so it remains a valid barrier.
FlatMask flatSkeletonize(const FlatMask& mask, int w, int h);

// Morphological closing (radius r px) + despeckle: seals pinholes and ragged
// texture in grainy strokes, drops isolated specks of `despeckle` px or fewer.
// A superset of the input line pixels is guaranteed.
FlatMask flatSmoothMask(const FlatMask& line, int w, int h, int r, int despeckle = 12);

// 1 where mask is 0 and vice versa -- the "distance to free space" input.
FlatMask flatInvertMask(const FlatMask& mask);

}  // namespace np
