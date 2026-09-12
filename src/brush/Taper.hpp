#pragma once

#include <vector>

#include "brush/StrokePath.hpp"

namespace np {

// brush/Taper -- one taper ramp, and the formula both ends of a stroke share
// (`NativeBrush::taperIn`/`taperOut`), pulled out as a pure function so it
// can be asserted directly rather than only through a real stroke's dab
// footprint.
//
// `StrokeSession::depositPending()` is the one caller. It measures the ramp
// from the near END of the stroke in both directions: the entry taper reads
// a dab's own `distanceTravelled_` (arc length since the stroke's first dab,
// 0 at the origin dab), the exit taper reads the arc length from that dab to
// the LAST one -- which is why the exit taper needs `StrokeSession`'s
// hold-back (that header's own section on it) and the entry taper does not.
struct BrushTaper {
  // The length is kept when this is off, so a taper switched off and back on
  // comes back the way it was set -- `lengthPx <= 0` is "no ramp", not "off".
  bool on = false;
  // Arc length, canvas px, over which radius (and flow, if `flow`) ramps
  // between `minSizePct` and full.
  float lengthPx = 0.0f;
  // What fraction (0-100%) of full size the tip of the ramp is. 0 is a point.
  float minSizePct = 0.0f;
  bool flow = false;
};

bool brushTaperEqual(const BrushTaper& a, const BrushTaper& b) noexcept;

// 1.0 (the identity) whenever the taper is off or its length is <= 0 -- so a
// non-tapering brush's radius/flow are multiplied by exactly 1.0f, not merely
// close to it. `distanceFromEndPx` is measured from whichever end of the
// stroke this taper belongs to.
float taperMultiplier(float distanceFromEndPx, const BrushTaper& taper) noexcept;

// Splits the segments of a stroke's held-back tail until consecutive dabs sit
// no further apart than the spacing the TAPERED tip at that point wants --
// `spacingPx` is the full-size spacing, scaled by the same ramp that is about
// to shrink the dabs. Without this a tapering stroke breaks into dots exactly
// where it is finest: spacing is chosen when a dab is EMITTED, and an exit
// taper is not resolvable until the stroke ends (`StrokeSession::
// taperedSpacingPx()`'s own comment, and `heldBack_`'s). A no-op, leaving
// `tail` untouched, when the taper is off or has no length.
void subdivideTaperedTail(std::vector<StrokeDab>& tail, const BrushTaper& taper,
                          float spacingPx) noexcept;

}  // namespace np
