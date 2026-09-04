#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "flats/Field.hpp"
#include "flats/Gaps.hpp"
#include "flats/Sag.hpp"
#include "flats/Segment.hpp"

// flats/Model -- what a Flats layer STORES, and how it becomes fills.
// Ported from autoFlats src/state.ts plus the algorithm that lived in its
// main.ts (replayEdits, regionAnchor, applyMergePair, applyMergeStroke,
// applyShapeFill, assignGroups, regionAdjacency, applyPalette, carve,
// clusterSmall) -- docs/autoflats-migration.md §3's "800 lines of algorithm
// in a UI file", extracted here as the headless library that document says
// to build first.
//
// ==========================================================================
// 1. Store intent, replay it: the class-C model
// ==========================================================================
//
// A Flats layer holds no pixels. It holds `FlatsContent`: the segmentation
// PARAMETERS, and every repair the artist made recorded as WHERE THEY DREW --
// a merge as the two points clicked or the stroke dragged, a deletion as a
// marker position, a group as its lasso path, a recolour as a point inside
// the fill, a bridge as its polyline. Never as which region ids it hit,
// because a re-flat renumbers every region.
//
// `flatEvaluate()` is then a PURE FUNCTION of (the line art beneath, the
// content): segment, then replay every edit against the fresh regions in a
// fixed order. That is exactly the Strokes layer's contract (DESIGN-imaging.md
// §4 class C) and it is what makes editing the line art re-flat the drawing
// (PRD N2): the layer has nothing to go stale.
//
// One consequence worth stating because autoFlats did it differently: there
// is NO overlap-matching of colours and names from the previous evaluation
// (autoFlats' `matchColors`). That carried state forward from history, which
// would make what a document shows depend on the order things were done in
// rather than on what it stores. Here a fill's automatic colour comes from
// its anchor (§2), and a chosen colour, name or visibility is a recorded
// edit at that anchor -- so the same document always evaluates to the same
// fills.
//
// ==========================================================================
// 2. Determinism is a UX invariant (PRD N4; migration doc §5.2)
// ==========================================================================
//
// The automatic colour of a fill is derived from a point INSIDE it (its
// anchor: the centroid, snapped to the nearest pixel that is actually in the
// fill), not from its id. Ids are handed out by the segmenter and reshuffle on
// any parameter change; an id-derived palette repainted the whole drawing
// whenever a slider moved and made it impossible to see what had actually
// changed. Anchored to a place, nudging a slider from 417 fills to 381 leaves
// 91% wearing the colour they had, and putting the slider back reproduces all
// 417 exactly. Anchor rather than centroid because a ring and its hole share
// a centroid; and anchors are pixels in disjoint regions, so no two fills can
// collide. app/selftest/Flats.cpp holds this as a regression test.
//
// ==========================================================================
// 3. Colour is 8-bit display RGB here, deliberately
// ==========================================================================
//
// A fill's colour is an `std::array<uint8_t, 3>` in display sRGB, the domain
// the palette, the anchor hash and the graph colouring were all authored in.
// The Flats layer is RGB-kind colour, not Pigment (CONTEXT.md): flat colour
// does not mix. Conversion to the linear working space happens once, when
// the evaluation is rendered into tiles (app/FlatsLayer), through
// color/Space's srgbDecode() -- the same place the paint bucket decodes its
// swatch (ui/MacPaintUI.cpp's `foregroundLinearRgba`).

namespace np {

using FlatRgb = std::array<uint8_t, 3>;

// Segmentation parameters. Defaults are autoFlats' shipped defaults
// (index.html), which its README calls "new defaults" after tuning on the
// seven samples: the rubber sheet ON at 2 px, tight closures sealed first.
struct FlatParams {
  float lineThreshold = 0.05f;  // ink darker than this is line (0..1)
  float colourReject = 0.30f;   // saturation above this is not line (0..1)
  int smoothing = 1;            // morphological closing radius, px
  bool skeletonize = false;     // erode the line mask to a 1px centreline first
  int gapSize = 8;              // trapped-ball radius / widest plausible break, px
  float sheet = 2.0f;           // rubber-sheet persistence tau, px; 0 = trapped ball
  bool closeTightGaps = true;   // seal unambiguous breaks before segmenting
  int minRegion = 0;            // fills smaller than this are absorbed, px²
  int sliverWidth = 3;          // corridor slivers thinner than this merge, px
  int declutter = 50;           // hatching/texture absorption strength, 0..100
  bool autoMergeLeaks = true;   // fronts: merge open cross-flow fragments (ball mode only)
  int paletteSize = 0;          // 0 = unique colour per fill; else graph-coloured
  bool completionField = false; // reserved: the stochastic completion field scorer
};

bool operator==(const FlatParams& a, const FlatParams& b) noexcept;
inline bool operator!=(const FlatParams& a, const FlatParams& b) noexcept { return !(a == b); }

// ---- recorded edits: geometry, never region ids -------------------------

// A bridge (barrier) stroke: pinned into the segmentation mask like ink,
// never rendered, never exported. `erase` strokes clear a fat disc.
struct FlatBridgeStroke {
  uint32_t id = 0;
  FlatPolyline pts;
  bool erase = false;
};
constexpr float kFlatEraseRadius = 6.f;

// The two-click merge: the two points, not the two ids they resolved to.
struct FlatMergePair { uint32_t id = 0; float ax = 0, ay = 0, bx = 0, by = 0; };
// Draw-merge: every fill the stroke crosses merges into the one under its start.
struct FlatMergeStroke { uint32_t id = 0; FlatPolyline pts; };
// A fill the user removed: not rendered, not exported.
struct FlatDeleteMark { uint32_t id = 0; float x = 0, y = 0; };
// A fill the user drew by hand. Stamped after segmentation and wins over
// whatever the segmenter put there, because it was drawn on purpose.
struct FlatShapeFill { uint32_t id = 0; FlatPolyline pts; FlatRgb color{}; std::string name; };
// A user-drawn grouping, as the lasso path; membership is recomputed each flat.
struct FlatGroup { uint32_t id = 0; std::string name; FlatPolyline path; };
// A colour the user chose, as a point in the drawing. `slot` is the palette
// swatch it came from (-1 when it came from the well), and the link is live:
// adjust that swatch and every fill painted from it follows (PRD N6).
struct FlatRecolor { uint32_t id = 0; float x = 0, y = 0; int slot = -1; FlatRgb color{}; };
// A name or visibility the user set, as a point in the drawing.
struct FlatFillNote { uint32_t id = 0; float x = 0, y = 0; std::string name; bool visible = true; };
// The bucket's "carve": a new fill cut out of a larger one (the background,
// usually) at a click, with the current gap size as the ball radius.
struct FlatCarve { uint32_t id = 0; float x = 0, y = 0; };

struct FlatEdits {
  std::vector<FlatBridgeStroke> bridges;
  std::vector<FlatCarve> carves;
  std::vector<FlatMergeStroke> mergeStrokes;
  std::vector<FlatMergePair> mergePairs;
  std::vector<FlatDeleteMark> deleteMarks;
  std::vector<FlatShapeFill> shapeFills;
  std::vector<FlatGroup> groups;
  std::vector<FlatRecolor> recolors;
  std::vector<FlatFillNote> notes;
  uint32_t nextId = 1;
  uint32_t nextGroup = 1;
  bool empty() const noexcept;
};

// Everything a Flats layer stores. The palette is a fixed-length grid with
// holes: position is meaning (skin in the top row, cloth in the next), so
// clearing a swatch leaves a gap rather than shuffling everything up.
struct FlatsContent {
  FlatParams params;
  FlatEdits edits;
  std::vector<std::optional<FlatRgb>> palette;
};

// A hash over everything that affects the evaluated fills, so a raster cache
// keyed on it (core/VectorRaster's shape) cannot go stale. Floats by bit
// pattern, as `vectorContentHash()` does.
uint64_t flatsContentHash(const FlatsContent& c) noexcept;

// ---- the evaluation --------------------------------------------------------

struct FlatFill {
  int id = 0;
  FlatRgb color{};
  std::string name;
  bool visible = true;
  int parent = 0;   // region-level union-find; parent == id when root
  int area = 0;     // free (non-ink) pixels
  bool isBg = false;
  bool deleted = false;
  int group = 0;    // FlatGroup::id, 0 when ungrouped
  int swatch = -1;  // palette slot, -1 when not from the palette
};

struct FlatEvaluation {
  int w = 0, h = 0;
  FlatInk ink;                       // display-domain darkness
  FlatMask line;                     // the segmentation mask: thresholded ink + bridges
  FlatLabels core;                   // region ids on free pixels, 0 on ink
  FlatLabels labels;                 // core grown under the strokes; what is rendered
  std::vector<FlatFill> fills;       // indexed by id; [0] unused
  std::vector<FlatPolyline> suggestions;  // gap bridges proposed, best first
  FlatSegs closures;                 // breaks the segmenter sealed itself (display only)
  FlatSagView sag;                   // empty when the trapped ball ran

  int root(int id) const noexcept;
  std::vector<int32_t> rootLut() const;
  std::vector<int> roots() const;  // root ids, largest area first
  // The fill under a point, root-resolved; 0 off the image.
  int fillAt(float x, float y) const noexcept;
  // The anchor of every root: centroid snapped to the nearest pixel inside.
  std::vector<std::array<int, 2>> anchors() const;  // indexed by id; {-1,-1} when none
};

// The line mask for `content` over `ink`: threshold, smooth, OR in the
// bridges, optionally skeletonise. `includeBridges=false` gives the mask the
// gap finder should see (bridges already accepted must not hide new breaks).
FlatMask flatLineMask(const FlatInk& ink, int w, int h, const FlatsContent& content,
                      bool includeBridges = true);

// Rasterise the bridge strokes alone: 1px draw lines, fat erase discs.
FlatMask flatBridgeMask(const FlatEdits& edits, int w, int h);

// Segment + replay. `rgba8` is w*h*4 display-encoded bytes of the line art
// beneath the layer (see flats/Ink for the domain argument).
FlatEvaluation flatEvaluate(const uint8_t* rgba8, int w, int h, const FlatsContent& content);
// Same from a precomputed ink map (tests, and callers that cache the ink).
FlatEvaluation flatEvaluateInk(FlatInk ink, int w, int h, const FlatsContent& content);

// Paint the evaluation into straight-alpha display RGBA8: each visible,
// non-deleted root's colour over its labels; ink pixels take the colour of
// the fill grown under them so the line art overlays with no fringe.
void flatRenderRgba8(const FlatEvaluation& e, uint8_t* out);

// ---- the pieces, exposed for the tools and the tests ----------------------

// The automatic colour of a fill from its anchor. §2.
FlatRgb flatAnchorColor(int x, int y);
// The K-palette colour for slot i.
FlatRgb flatPaletteColor(int i);
// Neighbouring roots never share a colour: greedy graph colouring, largest
// first. K == 0 restores a unique colour per fill.
void flatApplyPalette(FlatEvaluation& e, int K);
// Root adjacency over `labels`, indexed by root id.
std::vector<std::vector<int>> flatRegionAdjacency(const FlatEvaluation& e);

// Replay every recorded edit against fresh regions, in the fixed order
// carves, merge strokes, merge pairs, deletions, shape fills, recolours,
// notes, groups. Called by flatEvaluate; exposed so a tool can re-run it
// after appending an edit without re-segmenting.
void flatReplayEdits(FlatEvaluation& e, const FlatsContent& content);

// The effect of one merge stroke: the fills it crossed merge into the one
// under its start. Returns the ids merged (empty with a reason when nothing
// happened) -- the tool reports the reason; the replay ignores it.
struct FlatMergeOutcome { int into = 0; std::vector<int> merged; const char* reason = nullptr; };
FlatMergeOutcome flatApplyMergeStroke(FlatEvaluation& e, const FlatPolyline& pts);
void flatApplyMergePair(FlatEvaluation& e, const FlatMergePair& p);
bool flatApplyShapeFill(FlatEvaluation& e, const FlatShapeFill& sf);
void flatAssignGroups(FlatEvaluation& e, const std::vector<FlatGroup>& groups);

// Carve a new trapped-ball region out of the fill under (x, y). False when
// no ball of radius r fits there.
bool flatCarveAt(FlatEvaluation& e, int x, int y, int r);

// Small fills with an un-inked (open) border to a neighbour -> merge pairs,
// as anchor-point pairs ready to be recorded. `maxArea` in px².
std::vector<FlatMergePair> flatClusterSmall(const FlatEvaluation& e, int maxArea);

// Nearest recorded edit to a point within `reach` px, for the select-edits
// tool. Kind: 0 none, 1 bridge, 2 merge stroke, 3 merge pair, 4 delete,
// 5 shape, 6 group, 7 carve.
struct FlatEditRef { int kind = 0; uint32_t id = 0; };
FlatEditRef flatEditAt(const FlatEdits& edits, float x, float y, float reach);
bool flatRemoveEdit(FlatEdits& edits, FlatEditRef ref);

// Geometry helpers shared with the tools.
bool flatPointInPoly(float x, float y, const FlatPolyline& p);
float flatDistToSeg(float px, float py, float x1, float y1, float x2, float y2);

// HSL <-> RGB in the 8-bit display domain, as the palette editor needs.
FlatRgb flatHslToRgb(float h, float s, float l);
std::array<float, 3> flatRgbToHsl(FlatRgb rgb);

}  // namespace np
