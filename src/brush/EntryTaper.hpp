#pragma once

namespace np {

// brush/EntryTaper -- Wave 2's entry taper formula (`NativeBrush::taperInPx`/
// `taperMinSize`), pulled out as a pure function so `app/selftest/
// BrushTaper.cpp` can assert it directly rather than only through a real
// stroke's dab footprint. `StrokeSession::depositPending()` is the one
// caller: `distanceTravelledPx` is that dab's own `distanceTravelled_` --
// arc length since the stroke's first dab, 0 at the origin dab.
//
// 1.0 (the identity) whenever `taperInPx <= 0` -- off, the default -- so a
// non-tapering brush's radius/flow are multiplied by exactly 1.0f, not
// merely close to it.
float entryTaperMultiplier(float distanceTravelledPx, float taperInPx,
                           float taperMinSizePct) noexcept;

}  // namespace np
