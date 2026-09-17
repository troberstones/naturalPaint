#pragma once

#include <vector>

#include "flats/Field.hpp"

// flats/Sag -- segmentation by where the rubber sheet pools.
// Ported from autoFlats src/core/sag.ts.
//
// Given the sag field (flats/Membrane), a drawn area is a VALLEY: the sheet
// hangs lowest somewhere inside it and climbs back to the pinned ink on every
// side. So: flood downhill from every local maximum of sag and let the basins
// meet. The dividing ridge between two basins is the watershed line, and where
// the ink is solid that ridge sits exactly on the stroke, because the stroke
// is pinned to zero -- the deepest possible ridge.
//
// The interesting case is a BROKEN stroke. Then the ridge cannot follow the
// ink all the way; it has to hop the gap through free space, and it crosses at
// a col whose height is the width of the gap. That col is the whole trick:
//
//   * a real gap is narrow, so the col is low, so the two basins are deep
//     relative to where they meet -- they are clearly two things, and the fill
//     does not leak across, even though nothing physically blocks it;
//   * a spurious split inside one area (the waist of a limb, a soft bulge) has
//     a col nearly as high as the basins themselves -- shallow, so it collapses.
//
// That difference is topological persistence: a basin survives if its peak
// stands more than `tau` above the col where it first meets a neighbour. One
// threshold, in pixels, decides both "is this gap real?" and "is this a
// separate area?" -- questions the trapped-ball path answers with separate
// machinery (gap radius, min area, sliver width, declutter).
//
// This is what the bucket's "Flats" mode (docs/adr/0009) uses to decide what
// one click fills: the region is a basin of this field, not a colour-tolerance
// flood.

namespace np {

class GpuContext;

struct FlatSagResult {
  FlatLabels core;             // region id per free pixel, 0 on ink
  std::vector<float> sag;  // roominess in px (diagnostic / reusable)
};

// tauPx  -- a basin standing less than this above its col is a bulge, not an area.
// maxGap -- the widest break the artist might plausibly have left; sets how far
//           the ridge may search for the ink that justifies a boundary.
// gpu    -- non-owning; non-null routes the sag solve through membraneSagGpu()
//           instead of the CPU flatMembraneSag(). Never set by a headless caller.
FlatSagResult flatSagSegment(const FlatMask& line, int w, int h, float tauPx, int maxGap,
                             GpuContext* gpu = nullptr);

// Registers the GPU membrane solve `flatSagSegment()` calls when it is given
// a non-null GpuContext. An explicit setter rather than the more usual
// static-initializer registration, so the wiring is one visible line in
// main.cpp instead of an implicit link-order dependency -- and because
// flats/MembraneGpu.cpp (wgpu-dependent) is deliberately never compiled into
// `flatstest` (docs/autoflats-migration.md: that target stays GPU-free), a
// hard call from here to membraneSagGpu() by name would make flatstest fail
// to link. flatstest never calls this setter, so the slot stays null there
// and flatSagSegment()'s `gpu` parameter -- always null in that binary too --
// never reads it.
using MembraneSagGpuFn = std::vector<float> (*)(GpuContext&, const FlatMask&, int, int, int, float);
void flatsSetMembraneSagGpu(MembraneSagGpuFn fn);

// Same, on a sag field already solved (the field only depends on the line
// mask, so a re-segment at a new tau can reuse it).
FlatLabels flatSagWatershed(const std::vector<float>& sag, const FlatMask& line, int w, int h, float tauPx,
                    int maxGap);

// Sag in px -> a byte per pixel for display, on a log ramp. Roominess spans
// orders of magnitude in one drawing -- open background sags 600px while the
// inside of a sleeve sags 15 -- so a linear ramp paints every figure the same
// near-black. `max` goes back with it so the scale can still be read in px.
struct FlatSagView {
  std::vector<uint8_t> data;
  float max = 0;
};
FlatSagView flatSagView(const std::vector<float>& sag);

}  // namespace np
