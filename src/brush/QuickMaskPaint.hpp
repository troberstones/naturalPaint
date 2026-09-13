#pragma once

#include <cstdint>

#include "brush/Deposit.hpp"
#include "core/Channels.hpp"

// brush/QuickMaskPaint -- one brush/eraser dab into a `core::QuickMask` (PRD
// E12).
//
// Shaped after brush/MaskPaint's footprint loop (`dabPixelBounds()` /
// `dabCoverage()`), but stateless: core/Channels.hpp's own rule is that a
// quick-mask brush stroke is an Add and an eraser a Subtract
// (`core::combineCoverage()`'s max/min), and both are already idempotent
// under overlap, so there is no ceiling or per-stroke accumulator to latch --
// unlike a layer mask's sample, which has no privileged end and needs one.
// Opacity folds straight into the per-dab weight for the identical reason:
// `max(base, flow*opacity*cov)` cannot climb past `flow*opacity` no matter
// how many dabs of a lingering stroke propose it, so the ceiling brush/
// RgbDeposit needs an accumulator to enforce falls out of `max`/`min` here
// for free.
//
// Deliberately not a `StrokeRoute`: a quick mask has no `Layer`, no
// `core::History` entry and no revision, so it does not answer
// `strokeRouteWritesLayer()` truthfully, and forcing it into that enum would
// model a session gesture as a document one. `sim::PaintSim` is this
// codebase's other paintable target outside `app::StrokeSession`; this
// follows that precedent, one level lighter.
namespace np {

// `erase` selects Subtract, otherwise Add. Returns the number of texels
// written -- 0 for a footprint off canvas, outside the tip's falloff, or
// already at the value this dab would leave it at (`paintQuickMask()`'s own
// no-op skip), none of which allocate.
size_t paintQuickMaskDab(QuickMask& mask, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                         int32_t canvasH, bool erase);

}  // namespace np
