#pragma once

#include <cstdint>
#include <vector>

// flats/Field -- the dense, full-frame buffers the flatting library works on.
//
// Everything in flats/ is a global algorithm over a dense label field
// (docs/autoflats-migration.md §5.3): trapped ball, watershed, the Poisson sag
// and the distance transforms all need the whole frame at once and cannot be
// evaluated per 128² tile. So this module deliberately does NOT use
// core/TileStore. A Flats layer allocates these transiently for one evaluation
// and releases them (PRD N11); what it keeps at rest is the label field,
// RLE-compressed per scanline (flats/FlatLabels).
//
// Pixel order is row-major, index = y * w + x, matching the autoFlats source
// this is ported from so that every line comment there still applies here.
//
// Domain note (docs/autoflats-migration.md §5.1, PRD N10): the `Ink` values
// are 8-bit DISPLAY-domain darkness, never linear light. Ink extraction is a
// perceptual threshold and must be taken where the artist made it; the caller
// (app/FlatsEvaluate) encodes the working-space composite through
// color/Space's srgbEncode() before handing it here.

namespace np {

using FlatMask = std::vector<uint8_t>;    // 0 / 1 per pixel
using FlatInk = std::vector<uint8_t>;  // 0..255 darkness per pixel
using FlatLabels = std::vector<int32_t>;  // region id per pixel, 0 = none
using FlatDist = std::vector<int32_t>;    // chamfer 3-4 distance, 3 ≈ 1 px

// A synthetic drawing for tests: the line mask plus the ink density that
// produced it. Mirrors autoFlats' test/harness.ts `FlatArt`.
struct FlatArt {
  int w = 0, h = 0;
  FlatMask line;
  FlatInk ink;
};

inline FlatArt flatBlankArt(int w, int h) {
  FlatArt a;
  a.w = w;
  a.h = h;
  a.line.assign(static_cast<size_t>(w) * h, 0);
  a.ink.assign(static_cast<size_t>(w) * h, 0);
  return a;
}

// A square stroke of width `wpx` from (x0,y0) to (x1,y1), marking both line
// and ink -- the harness's `stroke()`.
void flatArtStroke(FlatArt& a, int x0, int y0, int x1, int y1, int wpx = 2);
// A rectangle outline, optionally with a `gap`-px break centred in its left wall.
void flatArtRect(FlatArt& a, int x0, int y0, int x1, int y1, int gap = 0);

// Three closed boxes in open space: the simplest thing with a known answer.
FlatArt flatThreeBoxes(int w = 400, int h = 300);
// One box whose left wall has a break of `gap` px -- the case the whole
// feature exists for. Narrow gaps must not leak; wide ones may.
FlatArt flatLeakyBox(int gap, int w = 400, int h = 300);
// A box filled with parallel hatching: pockets between strokes are shading,
// not areas, and should not each become their own fill.
FlatArt flatHatchedBox(int w = 400, int h = 300);

}  // namespace np
