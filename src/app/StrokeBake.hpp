#pragma once
#include <cstddef>

#include "app/DocumentLifecycle.hpp"
#include "sim/PaintSim.hpp"

namespace np {

// The bake: the solver's deposited pigment, written into a layer's tiles.
//
// This is the third and last arithmetic piece of the stroke bridge, and it
// does no arithmetic of its own. `sim/PaintSim`'s occupancy reduction says
// *which* tiles; its deferred readback supplies *what is in them*;
// `core/PigmentBake` says what a solver texel *becomes*. This joins the three
// and writes the result, and it lives in `app/` for exactly that reason --
// `core/` must not know about the solver and `sim/` must not know about
// documents, so the one place that knows both is here.
//
// --- What it deliberately does not do -------------------------------------
//
// It does not decide *when*. Bake-on-dry is a cadence question and the caller
// owns it: `PaintSim::readTileOccupancy()` reports mass and wetness together
// precisely so a caller can apply its own rule and hand the chosen tiles here.
// Baking a still-wet tile is not refused -- a forced bake on undo-while-wet is
// a real case -- but it silently drops whatever is still suspended, so the
// result says how many tiles were wet at the time and the caller can decide
// whether that mattered.
//
// It does not clear the sim, and it does not record history. Both belong to
// the frame sequence around it, and folding either in here would make a bake
// that is *observed* impossible to write.

struct BakeResult {
  size_t tilesWritten = 0;
  size_t texelsWritten = 0;
  float peakCoverage = 0.0f;
  // Tiles whose solver texels were all below the mass floor. Not an error:
  // the occupancy pass names a tile when its *maximum* is worth baking, and a
  // tile can hold one loaded texel and 16383 empty ones. Reported so a caller
  // that expected paint and got none can tell that apart from a failure.
  size_t tilesEmpty = 0;
};

// Writes one tile of solver texels into `out`. `depC` and `depR` are each
// 128*128*4 floats in row order -- exactly what
// `PaintSim::pigmentReadbackDepC()` hands back. Returns the number of texels
// that carried enough mass to write.
//
// Texels below the floor are left untouched rather than written as
// transparent, which matters: a tile is allocated on write, and a bake that
// stamped zeros over an existing tile would erase paint that was already
// there instead of adding to it.
size_t bakePigmentTileFrom(const float* depC, const float* depR, float absorption,
                           PigmentTile& out);

// The whole bake, for a readback that is `Ready`. Walks the tiles the readback
// was issued for, converts each, and writes it into `layer`'s pigment tiles.
// Returns a zeroed result if the layer is not a Pigment layer or the readback
// is not ready -- both are caller errors, and neither can be repaired here.
BakeResult bakePigmentTiles(const PaintSim& sim, Layer& layer, float absorption);

}  // namespace np
