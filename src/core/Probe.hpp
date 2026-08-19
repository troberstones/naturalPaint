#pragma once

#include <array>
#include <cstdint>

#include "core/Document.hpp"
#include "core/Tile.hpp"

// core/Probe (PLAN.md "Phase 2 -- See a file", step 10: "Pixel probe
// reporting both linear and display values, and the eyedropper, which is
// the same sampling code writing to the foreground colour instead of a
// readout (PRD Q10). Sample size and sample-all-layers are parameters of
// the sample, not separate tools.").
//
// probePixel() below is that one sampling code: a pure, read-only Document
// query -- coordinate + parameters in, colour out -- with no UI attached.
// It is deliberately NOT the eyedropper tool itself: PRD Q10's "eyedropper
// picks into the foreground colour" needs a real foreground-colour concept
// to write to, and today's interactive brush state (app/AppState.hpp's
// BrushState) only holds a palette index, not an arbitrary RGB value --
// that's docs/ui.md §3.3's still-undesigned COLOR-panel chrome, out of
// scope here. A future pixel-probe readout and a future eyedropper are
// both meant to be trivial wrappers around probePixel() -- call it, then
// either display the ProbeSample or copy it into wherever a foreground
// colour ends up living -- not two separate sampling paths, exactly per
// step 10's own wording above.
namespace np {

// How much of the document one sample averages, and which layer(s) it
// reads. A real, first-class parameter of the sample (per step 10's "not
// separate tools") rather than a hardcoded 1x1 read.
struct ProbeParams {
  // Edge length, in document pixels, of the square box averaged around the
  // probed coordinate. 1 = a single texel ("point sample"); 3, 5, ...
  // average an NxN box, Photoshop's "3 by 3 Average" / "5 by 5 Average"
  // shape. Must be >= 1 -- probePixel() clamps anything <= 0 up to 1
  // rather than misbehaving on a bad caller value.
  //
  // Odd sizes centre exactly on the probed coordinate. Even sizes are
  // still handled (never a crash or UB) but the box is biased half a
  // texel toward +x/+y, because there is no ambiguity-free way to centre
  // an even-width box on a single integer coordinate; this reuses the same
  // floor-biased direction core/Tile.hpp's tileCoordAt()/tileLocalOffset()
  // already commit to for negative coordinates, rather than inventing a
  // second rounding convention.
  int32_t sampleSize = 1;

  // Sample every RGB-kind layer with populated tile storage and composite
  // them, vs. sample only `activeLayerIndex`.
  //
  // As of this writing core::Document (core/Document.hpp) only ever holds
  // one populated RGB layer -- "design for N, ship 1" -- and no layer
  // compositing/blend-mode implementation exists anywhere in this codebase
  // yet (that's a later phase). So today, with at most one candidate layer
  // in play, both modes reduce to the same sum -- see Probe.cpp's
  // probePixel() for exactly what "reduces to" means and why that's the
  // honest behaviour rather than a stubbed-in compositing model. The field
  // still exists as a real, meaningful parameter for once multi-layer
  // stacks and blending land.
  bool sampleAllLayers = false;

  // Which layer to sample when sampleAllLayers is false. core::Document has
  // no "active layer" concept yet -- that belongs to a future app-level
  // selection state, out of scope for this module -- so callers that track
  // one pass it here; callers that don't (e.g. --selftest, against a
  // single-layer Document) can rely on the default, index 0. Out-of-range
  // or non-RGB-kind indices are handled the same as "nothing there": a
  // fully transparent sample, never a crash.
  int32_t activeLayerIndex = 0;
};

// One probed colour, in both of PLAN.md step 10's required forms.
//
// Both are straight (un-premultiplied) alpha. core::Tile stores
// premultiplied colour (DESIGN-imaging.md §2 "Alpha: premultiplied
// (associated)"; io/ImageIO.cpp's writeDecodedImageIntoLayer premultiplies
// on write, rgb *= a). A probe reports "the colour at this point" the way
// a user would recognise it -- straight, not dimmed toward black at low
// alpha -- so probePixel() un-premultiplies when reading, mirroring at
// this read boundary the same split io/ImageDecode.hpp's header comment
// documents at the opposite (decode) boundary.
struct ProbeSample {
  // Linear-light, straight-alpha RGBA -- the colour as the working space
  // actually stores it, no transfer function applied. {0,0,0,0} (fully
  // transparent black) both when nothing has ever been painted at the
  // probed location and, per the un-premultiply guard above, as the
  // defined RGB for any fully-transparent result.
  std::array<float, 4> linear{0.0f, 0.0f, 0.0f, 0.0f};

  // linear's RGB channels run through color::srgbEncode() for display.
  // Alpha is carried through unchanged -- alpha is opacity, not light, and
  // is never gamma-encoded, the same policy io/ImageDecode.hpp's decode
  // side already documents for the inverse (encoded-file -> linear)
  // direction.
  std::array<float, 4> display{0.0f, 0.0f, 0.0f, 0.0f};
};

// Samples `doc` at document-pixel coordinate `at`, per `params`. Read-only:
// goes through TileStore::find(), never getOrCreate(), so probing an area
// nobody has painted allocates nothing. Never crashes -- an out-of-bounds
// coordinate, an unoccupied tile, an empty/no-RGB-layer Document, or an
// out-of-range activeLayerIndex all read back ProbeSample{}, fully
// transparent black in both linear and display form, the same implicit
// value core::Tile itself gives an unwritten texel.
ProbeSample probePixel(const Document& doc, PixelCoord at, const ProbeParams& params = {});

}  // namespace np
