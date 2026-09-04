#pragma once

#include <vector>

#include "flats/Field.hpp"

// flats/Gaps -- where the line work fails to close, and what to draw there.
// Ported from autoFlats src/core/{flow,relatability,fronts,gaps,curves,closure}.ts.
//
// Gap suggestion is grounded in vision science rather than ad-hoc thresholds
// (autoFlats DEVELOPMENT.md "Gestalt gap closing"):
//
//   good continuation  Kellman-Shipley relatability: a smooth, monotonic
//                      curve bending <= 90 deg with no inflection -- the GATE
//   -- shape/ranking   Euler elastica energy ∫(1 + βκ²)ds
//   similarity         stroke-width matching at the two anchors
//   parallelism        co-completion from a partner stroke that spans the gap
//   closure (regions)  a bridge must split one fill into two real parts
//   Prägnanz           the fewest bridges that achieve those closures
//
// Two sources propose bridges: region-collision FRONTS (a border between two
// fills that runs through open space is a leak, and the narrowest point of it
// is where the ink failed) and skeleton stroke ENDPOINTS paired by
// relatability. Every proposal then passes `flatSelectBridges()`, which is
// what makes the review list short and each entry worth a click.
//
// A `Bridge` is a polyline in image pixels, [x0,y0,x1,y1,...]. Accepting one
// records it as a barrier stroke (flats/Model), pinned into the line mask for
// segmentation and never rendered or exported (CONTEXT.md "Bridge").
//
// Terminology (CONTEXT.md §6 of the migration doc): the stroke-direction
// field is the **stroke orientation field** here, not "flow" -- `Flow` in
// this codebase means brush flow and nothing else.

namespace np {

using FlatPolyline = std::vector<float>;  // [x0,y0,x1,y1,...]
using FlatSegs = std::vector<float>;      // 4-tuples [x1,y1,x2,y2, ...]

// Stroke-orientation field via structure tensor of the ink map, at 1/4
// resolution (orientation varies slowly). fx/fy = unit stroke tangent (axial:
// v and -v are equivalent), coh = 0..1 how directional the strokes are there.
struct FlatOrientation {
  std::vector<float> fx, fy, coh;
  int w2 = 0, h2 = 0;
};
FlatOrientation flatOrientationField(const FlatInk& ink, int w, int h);
struct FlatOrientationSample { float fx, fy, coh; };
FlatOrientationSample flatSampleOrientation(const FlatOrientation& f, int x, int y);

// Gestalt "good continuation", formalised. Tangents are UNIT vectors pointing
// OUT of each stroke tip, into the gap.
struct FlatRelatability {
  bool ok = false;
  float energy = 0;  // +inf when !ok
  float bend = 0;    // radians
};
constexpr float kFlatElasticaBeta = 30.f;
FlatRelatability flatRelatable(float ax, float ay, float atx, float aty, float bx, float by, float btx,
                               float bty, float coneCos = 0.5f, float maxBendDeg = 90.f);
// Discrete elastica energy of a polyline: length plus a curvature penalty.
float flatElasticaEnergy(const FlatPolyline& poly, float beta = kFlatElasticaBeta);
// Minimal-elastica completion between two tips with known tangents (unit,
// into the gap); the chord when even the best curve bows too hard.
FlatPolyline flatElasticaCurve(float ax, float ay, float atx, float aty, float bx, float by, float btx,
                               float bty, int k = 0);

// Region-collision fronts. `isBg` indexed by region id, or null; `doMerge`
// enables auto-merge of open, cross-flow leak fragments.
struct FlatFrontsResult {
  std::vector<std::pair<int, int>> merges;
  FlatSegs segs;  // candidate bridges, best first
};
FlatFrontsResult flatAnalyzeFronts(const FlatLabels& labels, const FlatMask& line, int w, int h,
                                   const FlatOrientation& orient, int maxBridge,
                                   const std::vector<uint8_t>* isBg, bool doMerge);

// Skeleton-endpoint bridges: thin the mask, find stroke tips, pair nearby
// relatable tips (or tip -> nearby foreign stroke). With `labels`, keep only
// bridges through which the SAME fill flows -- gaps that actually leak.
FlatSegs flatSuggestGaps(const FlatMask& line, int w, int h, int maxGap, const FlatLabels* labels);

// The unambiguous breaks: short, tight, open. Closed before segmenting, in
// the segmentation mask only, so the sheet sees a closed shape. Deliberately
// mean -- guesses are what the review list is for.
FlatSegs flatTightClosures(const FlatMask& line, int w, int h, int maxGap);

// Bridge shaping: a parallel partner's wobble copied across when one spans
// the gap, else a flow-curved Hermite, else the chord.
FlatPolyline flatCoCompleteBridge(float x1, float y1, float x2, float y2, const FlatMask& line, int w,
                                  int h, bool* found);
FlatPolyline flatCurveBridge(float x1, float y1, float x2, float y2, const FlatOrientation* orient);
// 4-tuples -> shaped polylines.
std::vector<FlatPolyline> flatBridgePaths(const FlatSegs& segs, const FlatOrientation* orient,
                                          const FlatMask& line, int w, int h);

// Closure + Prägnanz: keep a non-redundant subset of `paths` (best-first).
// Returns kept indices and, per kept path, the closure gain (px² of the
// smaller side).
constexpr int kFlatMinSplit = 150;
struct FlatBridgeSelection {
  std::vector<int> keep;
  std::vector<int> gain;
};
FlatBridgeSelection flatSelectBridges(const std::vector<FlatPolyline>& paths, const FlatLabels& labels,
                                      const FlatMask& line, int w, int h, int minSplit = kFlatMinSplit);

// The widest bridge worth drawing for a given gap size.
inline int flatMaxBridge(int maxGap) { return maxGap * 2 + 4 > 6 ? maxGap * 2 + 4 : 6; }

// 1px, 8-connected Bresenham into a mask -- a closure or a bridge is pinned
// into the membrane like ink, so any extra width is roominess taken from the
// very areas it is trying to rescue.
void flatMarkLine(FlatMask& mask, int w, int h, float x0, float y0, float x1, float y1);

}  // namespace np
