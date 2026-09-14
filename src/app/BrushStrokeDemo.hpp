#pragma once

// app/BrushStrokeDemo -- `--brush-stroke-demo`, the canvas the golden harness's
// `brush_strokes` view photographs.
//
// Ten strokes, each painted by a real `StrokeSession` with every argument the
// canvas passes (model, native brush, Pigment buildup mode), so a behaviour
// that stops reaching the stroke shows up as its stroke changing. Left column:
//
//   1. entry and exit taper, size only
//   2. entry and exit taper with flow, to a 25% minimum
//   3. Wash on a Pigment layer, a scribble that crosses itself
//   4. Build Up on a Pigment layer, the same scribble
//   5. Shape Dynamics Size and Transfer Flow on pen pressure, 0 -> 1 -> 0
//
// Right column:
//
//   1. an elliptical tip whose angle follows the stroke's direction
//   2. Scattering, both axes, three dabs per position
//   3. Texture, a synthetic stripe paper blended Multiply
//   4. Dual Brush, its second tip scattered
//   5. Build-up, a pen held still for a second on a synthetic clock
//
// Rows 3 and 4 differ only in the mode, and 1 and 2 in the flow switch: the
// pairs make the difference itself the thing compared, as
// `--pigment-stroke-demo` does for Mix and Normal.
//
// Its own file rather than main.cpp's anonymous namespace because it is a
// fixture of ten brushes, not a few lines; like app/BrushSheet it builds
// nothing any user-facing path calls.

namespace np {

class MixboxLut;
struct OpenDocument;

// Adds a Pigment layer above layer 0 and paints the rows into `od`. Prints
// one line per row (dabs, texels) so the picture is checkable from the log.
void buildBrushStrokeDemo(OpenDocument& od, const MixboxLut& lut);

}  // namespace np
