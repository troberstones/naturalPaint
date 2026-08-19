#pragma once

#include <cstdint>
#include <vector>

#include "core/Document.hpp"
#include "core/Tile.hpp"

// core/Histogram (PLAN.md "Phase 3 -- Grade it", step 7: "Histogram over the
// visible region"). A pure, read-only Document/TileStore query, following
// core::Probe's (src/core/Probe.hpp) precedent exactly: a plain Params
// struct in, a plain Result struct out, one free function, no classes, no
// UI attached.
//
// -- Domain: display-encoded (sRGB), not scene-linear -----------------------
// This module deliberately bins in display-encoded values, not the working
// space's own scene-linear light. A histogram exists to be read visually, by
// a human judging exposure/levels -- and every mainstream image editor's
// histogram (Photoshop et al.) plots display-referred values for exactly
// that reason. Binning in raw scene-linear would crowd nearly everything
// into the first few percent of the range, the same failure ADR-0004
// describes for un-shaped curve authoring -- a histogram in that domain
// would be visually useless for the one thing it's for.
//
// This is a different, and reversible, decision from color::Shaper's own
// domain choice. Shaper's log domain is a *format-level* commitment (ADR-0004):
// saved curve control points are literally coordinates in that space, so
// changing it later breaks saved documents. A histogram is recomputed fresh
// from the tiles every time it's drawn -- nothing is ever saved in this
// domain -- so picking a different domain later has zero saved-document
// compatibility impact and doesn't need Shaper's escalation-level scrutiny.
namespace np {

// What region to histogram, which layer(s) to read, and at what resolution.
// Mirrors ProbeParams's (core/Probe.hpp) two-field layer-selection
// convention exactly, for API consistency across this codebase's core/
// query modules.
struct HistogramParams {
  // The region to histogram, in document pixel coordinates: min inclusive,
  // max exclusive (ordinary half-open rect convention). wholeDocument()
  // below is the "whole canvas" helper. A future caller could instead pass
  // the current viewport's bounds (app/ViewTransform.hpp's toCanvas() could
  // compute those from screen coordinates) -- per this step's "visible
  // region" framing -- but wiring that up is UI work outside this step's
  // scope, the same "narrow, Document-level, defer the UI wiring" pattern
  // this codebase already used for core::Probe (PLAN.md step 10) and
  // others.
  PixelCoord regionMin{0, 0};
  PixelCoord regionMax{0, 0};

  // Same semantics as ProbeParams::sampleAllLayers / ::activeLayerIndex
  // (core/Probe.hpp) -- see that struct's own doc comments for the full
  // reasoning, including why summing every RGB-kind layer and single-layer
  // selection reduce to the same sum under today's Document invariant of at
  // most one populated RGB layer.
  bool sampleAllLayers = false;
  int32_t activeLayerIndex = 0;

  // Bin resolution, shared by all four channel histograms below. 256 is the
  // conventional resolution for this kind of histogram (one bin per 8-bit
  // display level). computeHistogram() clamps anything <= 0 up to 1 rather
  // than misbehaving, mirroring ProbeParams::sampleSize's own clamp
  // convention.
  int32_t binCount = 256;

  // The whole document, {0,0} to {doc.width, doc.height} -- the region a
  // caller reaches for before any viewport-bounds wiring exists.
  static HistogramParams wholeDocument(const Document& doc) {
    HistogramParams params;
    params.regionMin = PixelCoord{0, 0};
    params.regionMax = PixelCoord{doc.width, doc.height};
    return params;
  }
};

// Four independent single-channel histograms -- R, G, B, and Luma -- sharing
// one result struct, not a 4D joint histogram: each qualifying pixel
// increments exactly one bin in each of the four arrays (four increments
// total per pixel).
struct HistogramResult {
  std::vector<uint64_t> r;
  std::vector<uint64_t> g;
  std::vector<uint64_t> b;
  std::vector<uint64_t> luma;

  // How many pixels contributed to any bin at all -- every alpha > 0 texel
  // inside the requested region, across every allocated tile overlapping
  // it. This is not the region's area: unallocated tiles and alpha <= 0
  // texels both contribute 0 here, per this module's binning rule (see
  // Histogram.cpp).
  uint64_t sampleCount = 0;
};

// Computes a display-domain histogram of `doc` over `params.regionMin`..
// `params.regionMax`, per `params`'s layer-selection fields.
//
// Read-only, and deliberately does not touch TileStore::find() at all:
// TileStore's own begin()/end() ("iterate only the tiles that exist",
// core/TileStore.hpp) is walked directly, each allocated tile's document-
// space box intersected against the requested region and skipped entirely
// on no overlap, so histogramming a large-but-mostly-empty region costs
// nothing proportional to the region's size -- only to what's actually
// resident. An unpainted/unallocated region of the canvas contributes
// nothing to any bin, not "black".
//
// Within an allocated tile, a texel with alpha <= 0 is skipped entirely --
// it increments no bin. This is simpler than core::Probe's premultiplied-
// box-sum-then-single-unpremultiply approach: a histogram bins each pixel
// independently rather than averaging into a box, so there is no "dilution
// vs. darkening" tradeoff to solve (see Probe.cpp's own header comment for
// that reasoning, which does NOT apply here) -- a histogram simply
// shouldn't count "no content" as a data point.
//
// For a qualifying texel: un-premultiply (straight = premultiplied / a),
// srgbEncode() each of R/G/B to reach the display domain described above,
// then bin R, G, B independently; Luma bins
// 0.2126*dispR + 0.7152*dispG + 0.0722*dispB (shaders/grayscale_blit.wgsl's
// own Rec.709 weights, re-used verbatim so the histogram's Luma channel and
// the grayscale preview agree on what "luma" means). Display values outside
// [0,1] (possible -- the working space is unclamped scene-linear, and
// srgbEncode() of a linear value > 1.0 can still exceed 1.0) clamp into the
// first/last bin, the standard "outliers pile into the end bins"
// convention.
HistogramResult computeHistogram(const Document& doc, const HistogramParams& params);

}  // namespace np
