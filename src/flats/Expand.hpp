#pragma once

#include <cstddef>

#include "flats/Field.hpp"

// flats/Expand -- multi-source label growth in chamfer-distance order.
// Ported from autoFlats src/core/expand.ts.
//
// Bucketed Dijkstra, 8-connected, 3-4 weights. Growing all regions
// simultaneously by true distance gives smooth, near-euclidean fronts that
// meet at the medial axis -- no Manhattan-BFS stair-stepping.
//
// This is the CPU path. docs/autoflats-migration.md §1.3 plans a WGSL
// chamfer-relaxation kernel for the same growth; when that lands it must
// produce the same labels as this function on every fixture in
// app/selftest/Flats.cpp, which is the contract that lets it be swapped in.

namespace np {

struct FlatGrowOpts {
  // Pixels labels may never grow into (the line mask), or null.
  const FlatMask* blocked = nullptr;
  // Growth budget in chamfer units (3 ≈ 1 px).
  int32_t maxCost = 0x7ffffffe;
  // Pixel indices allowed to grow. Labeled pixels NOT in seeds are fixed
  // obstacles-with-identity: never overwritten, never growing. If null, every
  // labeled pixel seeds.
  const int32_t* seeds = nullptr;
  size_t seedCount = 0;
  // Per-pixel extra weight (the ink map) -- a soft watershed: fronts pay to
  // cross dark pixels, so region boundaries snap to faint stroke remnants
  // instead of the geometric midpoint.
  const FlatInk* cost = nullptr;
};

void flatGrowLabels(FlatLabels& labels, int w, int h, const FlatGrowOpts& opts);

// Expand core labels into line pixels; adjacent regions race and meet at the
// stroke's darkest ridge (given ink) or its medial axis, so fills reach the
// middle of every line (no fringe).
FlatLabels flatExpandLabels(const FlatLabels& core, int w, int h, const FlatInk* ink);

}  // namespace np
