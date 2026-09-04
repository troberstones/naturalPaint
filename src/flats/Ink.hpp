#pragma once

#include <cstdint>

#include "flats/Field.hpp"

// flats/Ink -- "is this pixel line art?", as a density and then as a mask.
//
// Ported from autoFlats src/core/ink.ts. Dark AND desaturated counts as line:
// alpha-only line art (RGB black, alpha = strokes) is composited over white
// first, and a coloured underdrawing (red construction lines, say) is rejected
// through its saturation. Every constant here was tuned against 8-bit sRGB
// values, which is why the input is display-encoded RGBA8 and not the
// linear working space (docs/autoflats-migration.md §5.1).

namespace np {

// rgba8 is w*h*4 bytes, straight (non-premultiplied) alpha, display-encoded.
// satTol is the saturation (0..1) above which darkness starts to be discounted.
FlatInk flatExtractInk(const uint8_t* rgba8, int w, int h, float satTol);

// ink > thr*255 -> 1.
FlatMask flatThresholdInk(const FlatInk& ink, float thr);

}  // namespace np
