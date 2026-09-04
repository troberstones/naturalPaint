#pragma once

#include <optional>
#include <vector>

#include "flats/Field.hpp"

// flats/Segment -- from a line mask to a compact label field.
// Ported from autoFlats src/core/{flatTrappedBall,regions,slivers,flatDeclutter}.ts.
//
// The stages run in this order (autoFlats DEVELOPMENT.md "The fill pipeline"):
//   flatTrappedBall  -> core     region ids on free pixels, 0 on ink
//   flatExpandLabels -> labels   core grown under the strokes (flats/Expand)
//   flatFinalizeRegions          absorb tiny regions, compact ids, flag background
//   flatMergeSlivers             corridor slivers between parallel strokes
//   flatDeclutter                hatching / texture fragments into what they shade
// Each mutates `core` and `labels` in place and leaves them consistent: a
// pixel's core id and label id are the same region wherever core is non-zero.

namespace np {

struct FlatTrappedBallResult {
  FlatLabels core;
  int count = 0;  // highest label id in use
};

// Multi-radius trapped-ball segmentation. A ball of radius r cannot pass a
// gap narrower than ~2r, so seeding where the ball fits (dist > r) and
// growing back by r fills regions without leaking through gaps. Descending
// radii: large safe areas first, small details last. Leftover free pixels
// (bands hugging strokes, wedges) are then assigned to their nearest connected
// region -- never fragmented into slivers -- and only truly enclosed pockets
// become new regions.
// attach=false skips the final leftover-attachment growth and pocket
// labelling (a GPU growth path substitutes its own, then calls flatLabelPockets).
FlatTrappedBallResult flatTrappedBall(const FlatMask& line, int w, int h, int maxGap, const FlatInk* ink,
                              bool attach = true);

// Enclosed pockets unreachable from any region become their own regions.
// Returns the highest label id in use.
int flatLabelPockets(FlatLabels& core, const FlatMask& line, int w, int h);

struct FlatRegionInfo {
  int id = 0;
  int area = 0;  // free (non-ink) pixels only
  bool isBg = false;  // touches the image frame
};

struct FlatFinalizeResult {
  std::vector<FlatRegionInfo> regions;  // sorted by area, largest first
  int count = 0;
};

// Absorb regions smaller than minArea into the neighbour sharing the most
// boundary, then compact ids to 1..K. Mutates core & labels in place.
FlatFinalizeResult flatFinalizeRegions(FlatLabels& core, FlatLabels& labels, int w, int h, int minArea);

// Merge "corridor slivers": regions that are thinner than sliverW everywhere
// (their max distance to line art is small) are the space between parallel /
// double-drawn strokes, not intentional cells. Each merges into the neighbour
// it shares the most OPEN border with (the area the corridor opens into),
// falling back to the longest border overall. Returns whether anything merged;
// the caller re-runs flatFinalizeRegions to compact ids.
bool flatMergeSlivers(FlatLabels& core, FlatLabels& labels, const FlatMask& line, int w, int h, int sliverW);

struct FlatDeclutterOpts {
  int maxArea;
  float maxMeanD;
  float minDensity;
};

// strength 0..100 -> thresholds. 0 disables the pass entirely (nullopt).
std::optional<FlatDeclutterOpts> flatDeclutterOpts(int strength);

// Figure/ground cleanup for dense line work: a fill that is small AND squeezed
// between strokes AND sitting in an inky neighbourhood is shading, not a drawn
// area, so it is absorbed into the surface it shades. Touching fragments
// collapse together first, so a whole hatched patch merges as one unit.
// isBg, if given, is indexed by region id. Returns whether anything merged.
bool flatDeclutter(FlatLabels& core, FlatLabels& labels, const FlatMask& line, int w, int h,
               const std::optional<FlatDeclutterOpts>& opts, const std::vector<uint8_t>* isBg);

// isBg lookup (indexed by id) from a finalize result.
std::vector<uint8_t> flatBackgroundLut(const std::vector<FlatRegionInfo>& regions);

// Apply region-pair merges (a,b) by union-find over ids, rewriting core and
// labels to the roots. Used by front analysis auto-merge and by tests.
void flatApplyMerges(FlatLabels& core, FlatLabels& labels, const std::vector<std::pair<int, int>>& merges);

}  // namespace np
