#pragma once

namespace np {

// MacPaint's proportions and hard edges, inverted for a dark room.
// The original was pure 1-bit black-on-white; a literal recolour looks wrong, so
// what carries over is the geometry — square everything, 1px borders, a chunky
// two-column tool grid, and a pattern strip pinned to the bottom.
void applyMacPaintDarkTheme();

}  // namespace np
