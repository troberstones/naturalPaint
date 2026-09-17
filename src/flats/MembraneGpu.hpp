#pragma once

#include <vector>

#include "flats/Field.hpp"

namespace np {

class GpuContext;

// GPU port of flats/Membrane.cpp's `flatMembraneSag()` -- same algorithm
// (geometric multigrid V-cycle, red-black Gauss-Seidel, full-weighting
// restriction, bilinear prolongation, and the A-norm line search on the
// coarse correction), same problem, executed on the device instead of the
// host. Numerically equivalent to the CPU solver within the same tolerances,
// not bit-exact -- red-black colouring here is (x+y) parity per-pixel rather
// than the CPU's row-banded ordering, and float summation order in the
// multigrid dot products differs between a serial host loop and a GPU
// tree reduction.
//
// Not wired into flatEvaluate()/FlatsLayer yet -- this is the solver and a
// way to check it, nothing more. See flats/Membrane.hpp's own header comment
// (docs/autoflats-migration.md §1.2) for why this exists.
//
// No float atomics on wgpu (and none reliable across the iOS/Metal backend
// this app also targets), so restriction is a GATHER over the coarse grid,
// prolongation is a GATHER over the fine grid (already naturally one), and
// the two line-search dot products plus the residual norm are each a
// per-workgroup tree reduction (the same idiom shaders/tile_occupancy.wgsl
// and sim/PaintSim.cpp already use) finished on the CPU after a small
// buffer readback. See shaders/membrane_*.wgsl.
std::vector<float> membraneSagGpu(GpuContext& gpu, const FlatMask& line, int w, int h,
                                  int cycles = 30, float tol = 1e-2f);

}  // namespace np
