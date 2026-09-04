#pragma once

#include <vector>

#include "flats/Field.hpp"

// flats/Membrane -- the drawing as a rubber sheet.
// Ported from autoFlats src/core/membrane.ts.
//
// Pin the sheet to the frame everywhere there is ink, let gravity pull the
// rest down, and read off how far each point sagged. That is a Poisson
// problem:
//
//     -laplace(u) = 1   on free space,    u = 0  on ink and at the image border
//
// Sag grows with the SQUARE of how much room there is -- an infinite strip of
// width w reaches u = w²/8 at its centre -- so we return sqrt(8u), which puts
// the answer back in pixels: in the middle of a w-wide channel it reads ~w,
// and in a disc of radius R it reads ~1.4R. Call it the local "roominess".
//
// Why bother, when the distance transform also measures roominess? Because
// distance is only C0. It ridges along the entire medial axis, so a watershed
// on it shatters every limb into a basin per bump. The membrane is smooth, and
// for a convex region sqrt(u) is concave (Makar-Limanov), which means a convex
// region has exactly ONE maximum -- basins land on drawn areas, not on noise.
// And roominess is quadratic in width, so a narrow leak between two areas sits
// in a deep col that separates them, while the same leak barely dents a
// distance field. That quadratic contrast is what closes gaps.
//
// Solved with a geometric multigrid V-cycle (red-black Gauss-Seidel smoother,
// full-weighting restriction, bilinear prolongation, and a line search on the
// coarse correction -- see the .cpp for why that last one is load-bearing) so
// cost is linear in pixels.
//
// This is the CPU solver. docs/autoflats-migration.md §1.2 observes that the
// same Poisson solve already runs on the GPU as the watercolour pressure
// projection (shaders/jacobi.wgsl); a `membraneSagGpu` with Dirichlet
// boundaries at ink is the planned replacement, held to the analytic strip
// and disc tests in app/selftest/Flats.cpp.

namespace np {

// Sag height in pixels: ~the width of the channel at that point, 0 on ink.
// `cycles` is a ceiling, not a setting: the solve stops as soon as the
// residual has dropped by `tol` relative to the start.
std::vector<float> flatMembraneSag(const FlatMask& line, int w, int h, int cycles = 30, float tol = 1e-2f);

}  // namespace np
